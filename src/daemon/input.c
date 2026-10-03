#include "os.h"

#include <limits.h>
#include "device.h"
#include "input.h"
#include "notify.h"
#include <assert.h>
#include <string.h>

#define IS_SCROLLWHEEL_V(scan)  ((scan) == BTN_WHEELUP   || (scan) == BTN_WHEELDOWN)
#define IS_SCROLLWHEEL_H(scan)  ((scan) == BTN_WHEELLEFT || (scan) == BTN_WHEELRIGHT)
#define IS_VOLWHEEL(scan)      (((scan) == KEY_VOLUMEUP  || (scan) == KEY_VOLUMEDOWN) && DEV_HAS_VOLWHEEL(kb))
// Lookup control wheel events by index in keymap. Alternatively one could check for non-null key->name to start with "ctrlwheel".
// FIXME: Replace by proper lookup of map in keymap.
#define IS_CTRLWHEEL(kb, index) ((kb)->protocol == PROTO_BRAGI && ((index) == 116 || (index) == 130))

static int macromask(const uchar* keys, const uchar* macro){
    // Scan a macro against key input. Return 0 if any of them don't match
    for(int i = 0; i < N_KEYBYTES_INPUT; i++){
        if((keys[i] & macro[i]) != macro[i])
            return 0;
    }
    return 1;
}

// FIFO operations require mmutex2. Enqueue before creating the thread, so
// scheduling cannot reverse trigger order. Removal also handles cancelled waiters.
static void macro_enqueue(usbdevice* kb, macro_param* param){
    if(kb->macro_tail)
        kb->macro_tail->next = param;
    else
        kb->macro_head = param;
    kb->macro_tail = param;
}

static void macro_remove(usbdevice* kb, macro_param* param){
    macro_param** link = &kb->macro_head;
    macro_param* previous = NULL;
    while(*link && *link != param){
        previous = *link;
        link = &(*link)->next;
    }
    assert(*link == param);
    *link = param->next;
    if(kb->macro_tail == param)
        kb->macro_tail = previous;
    pthread_cond_broadcast(mvar(kb));
}

// Called with imutex held. Destruction transfers ownership to the worker.
static void abort_macro(macro_param* param){
    usbdevice* kb = param->kb;
    pthread_mutex_lock(mmutex2(kb));
    param->abort = 1;
    pthread_cond_broadcast(mvar(kb));
    pthread_mutex_unlock(mmutex2(kb));
    pthread_cond_broadcast(mintvar(kb));
}

void stop_macros(usbdevice* kb){
    kb->macro_stopping = 1;
    pthread_mutex_lock(mmutex2(kb));
    for(macro_param* p = kb->macro_head; p; p = p->next)
        p->abort = 1;
    pthread_cond_broadcast(mvar(kb));
    pthread_cond_broadcast(mintvar(kb));
    queued_mutex_unlock(imutex(kb));
    while(kb->macro_head)
        pthread_cond_wait(mvar(kb), mmutex2(kb));
    pthread_mutex_unlock(mmutex2(kb));
    queued_mutex_lock(imutex(kb));
}

// Default macro keystroke delay
#define NANOSECONDS_PER_MILLISECONDS 1000000
const struct timespec macrodelay = { .tv_nsec = 1 * NANOSECONDS_PER_MILLISECONDS };
// Default and max values for macro repetition delays/debounces
#define REPETITION_DEFAULT_VALUE 500
#define REPETITION_MAX_VALUE 2000

// Interruptible action delay. imutex is held on entry/return; a wake-up caused
// by a trigger must not shorten the requested delay. Split long waits because
// queued_cond_nanosleep accepts a uint32_t nanosecond duration.
static void macro_delay(macro_param* param, uint32_t us){
    usbdevice* kb = param->kb;
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now))
        return;
    uint64_t deadline = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec + (uint64_t)us * 1000;
    while(!param->abort){
        if(clock_gettime(CLOCK_MONOTONIC, &now))
            break;
        uint64_t current = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
        if(current >= deadline)
            break;
        uint64_t remaining = deadline - current;
        queued_cond_nanosleep(mintvar(kb), imutex(kb), remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
    }
}

static void* play_macro(void* param){
    macro_param* ptr = param;
    usbdevice* kb = ptr->kb;
    keymacro* macro = ptr->macro;
#ifdef OS_MAC
    pthread_setname_np("ckb macro");
#endif
    pthread_mutex_lock(mmutex2(kb));
    while(kb->macro_head != ptr && !ptr->abort)
        pthread_cond_wait(mvar(kb), mmutex2(kb));
    pthread_mutex_unlock(mmutex2(kb));

    // Track keys emitted by this playback, including mouse buttons, so an
    // interrupted sequence does not leave its modifiers/buttons held down.
    short* pressed = calloc(macro->actioncount, sizeof(*pressed));
    int pressed_count = 0;
    int first_keydownloop = 1;
    queued_mutex_lock(imutex(kb));
    if(!pressed)
        macro->triggered = 0;
    while(pressed && !ptr->abort){
        for(int a = 0; a < macro->actioncount && !ptr->abort; ++a){
            macroaction* action = macro->actions + a;
            queued_mutex_lock(mmutex(kb));
            if(action->rel_x || action->rel_y){
                os_mousemove(kb, action->rel_x, action->rel_y);
                os_inputsync(kb, 0, 1);
            } else {
                if(IS_SCROLLWHEEL_V(action->scan)){
                    if(action->down)
                        os_mousescroll(kb, 0, action->scan == BTN_WHEELUP ? 1 : -1);
                } else if(IS_SCROLLWHEEL_H(action->scan)){
                    if(action->down)
                        os_mousescroll(kb, action->scan == BTN_WHEELLEFT ? -1 : 1, 0);
                } else {
                    os_keypress(kb, action->scan, action->down);
                    int i = 0;
                    while(i < pressed_count && pressed[i] != action->scan) ++i;
                    if(action->down && i == pressed_count)
                        pressed[pressed_count++] = action->scan;
                    else if(!action->down && i < pressed_count)
                        pressed[i] = pressed[--pressed_count];
                }
                int mouse = action->scan & SCAN_MOUSE;
                os_inputsync(kb, !mouse, mouse);
            }
            queued_mutex_unlock(mmutex(kb));
            if(action->rel_x || action->rel_y)
                continue;
            uint32_t delay_us = 1000;
            if(action->delay != UINT32_MAX && action->delay){
                if(action->delay_max){
                        // Generate a random delay in the range requested
                        // First, we need a random number that can be as large as uint32_t
                        // RAND_MAX can be as low as 0x7fff
                        uint16_t second = rand() & 0x7fff;
                        uint16_t first = rand() & 0x7fff;
                        uint32_t final = ((uint32_t)first << 17);
                        final |= (second << 1);
                        // Now that we have a sufficently large random number, range it appropriately
                        final = ((uint64_t)action->delay + (uint64_t)final) % (action->delay_max - action->delay);
                        delay_us = final;
                } else
                    delay_us = action->delay;
            }
            macro_delay(ptr, delay_us);
        }
        if(ptr->abort)
            break;
        uint32_t delay_ns = (first_keydownloop ? macro->repetition_debounce_ms : macro->repetition_delay_ms)
            * NANOSECONDS_PER_MILLISECONDS;
        if(macro->triggered > 1){
            macro->triggered -= 2;
            delay_ns = 0;
            first_keydownloop = 1;
        }
        if(!macro->triggered)
            break;
        if(!delay_ns)
            continue;
        queued_cond_nanosleep(mintvar(kb), imutex(kb), delay_ns);
        if(ptr->abort)
            break;
        if(macro->triggered == 2){
            macro->triggered = 0;
            break;
        }
        if(macro->triggered == 1)
            first_keydownloop = 0;
    }
    if(ptr->abort){
        queued_mutex_lock(mmutex(kb));
        while(pressed_count){
            int scan = pressed[--pressed_count];
            os_keypress(kb, scan, 0);
            os_inputsync(kb, !(scan & SCAN_MOUSE), scan & SCAN_MOUSE);
        }
        queued_mutex_unlock(mmutex(kb));
    }
    macro->param = NULL;
    // Only removed bindings transfer ownership. Shutdown cancels the retained
    // ones too; the profile frees those after stop_macros has drained the FIFO.
    if(ptr->removed){
        free(macro->actions);
        free(macro);
    }
    free(pressed);
    queued_mutex_unlock(imutex(kb));

    pthread_mutex_t* queue_mutex = mmutex2(kb);
    pthread_mutex_lock(queue_mutex);
    macro_remove(kb, ptr);
    free(ptr);
    // No access to kb after this unlock: closeusb may now recycle the device.
    pthread_mutex_unlock(queue_mutex);
    return NULL;
}

// imutex is held by inputupdate_keys; failure keeps the binding and no worker.
static void start_macro(usbdevice* kb, keymacro* macro){
    macro_param* params = calloc(1, sizeof(*params));
    if(!params){
        macro->triggered = 0;
        ckb_err("Could not allocate macro playback");
        return;
    }
    params->kb = kb;
    params->macro = macro;
    macro->param = params;
    pthread_mutex_lock(mmutex2(kb));
    macro_enqueue(kb, params);
    pthread_t thread;
    int retval = pthread_create(&thread, NULL, play_macro, params);
    if(retval){
        macro->triggered = 0;
        macro->param = NULL;
        macro_remove(kb, params);
        free(params);
        ckb_err("Could not create macro thread: %s", strerror(retval));
    } else {
        pthread_detach(thread);
#ifndef OS_MAC
        pthread_setname_np(thread, "ckb macro");
#endif
    }
    pthread_mutex_unlock(mmutex2(kb));
}

// Checks if the macro mask contains any wheels to prevent it from looping endlessly
static inline int is_wheel_keybit(const usbdevice* kb, const uchar* macro){
    for(int byte = 0; byte < N_KEYBYTES_INPUT; byte++){
        // Most entries are probably going to be 0, so skip over them
        if(!macro[byte])
            continue;
        // Go through each bit
        for(int bit = 0; bit < 8; bit++){
            int keyindex = byte * 8 + bit;
            if(!((macro[byte] >> bit) & 1))
                continue;
            // Get the index of the item and look it up in the keymap
            const key* ckey = kb->keymap + keyindex;
            // If there's at least a single wheel, return true
            if(IS_VOLWHEEL(ckey->scan) || IS_SCROLLWHEEL_V(ckey->scan) || IS_SCROLLWHEEL_H(ckey->scan) || IS_CTRLWHEEL(kb, keyindex))
                return 1;
        }
    }
    return 0;
}

///
/// \brief inputupdate_keys Handle input from Keyboard or mouse; start Macro if detected.
/// \param kb
///
static inline void inputupdate_keys(usbdevice* kb, int* sync_kb, int* sync_mouse){
    usbmode* mode = kb->profile->currentmode;
    binding* bind = &mode->bind;
    usbinput* input = &kb->input;

    // Don't do anything if the state hasn't changed
    if(!memcmp(input->prevkeys, input->keys, N_KEYBYTES_INPUT))
        return;
    // Look for macros matching the current state
    if (kb->active && !kb->macro_stopping) {
        for (int i = 0; i < bind->macrocount; i++) {
            keymacro* macro = bind->macros[i];
            // see the definition of keymacro.triggered in structures.h
            if (macromask(input->keys, macro->combo)) {
                if(!(macro->triggered & 1)){
                    macro->triggered++;
                    // Because there are no keyup events for wheels, pretend we received them
                    // to prevent endless looping
                    if(is_wheel_keybit(kb, macro->combo))
                        macro->triggered++;
                } else
                    continue;
            } else {
                if((macro->triggered & 1)) {
                    macro->triggered++;
                    pthread_cond_broadcast(mintvar(kb));
                }
                continue;
            }

            if (macro->triggered <= 3) {
                // start up a thread if there isn't already one running
                if (!macro->param) {
                    start_macro(kb, macro);
                } else {
                    // if there is already a thread running, it may be waiting for DELAY_REPEAT_*
                    // it does not need to wait anymore
                    pthread_cond_broadcast(mintvar(kb));
                }
            }
        }
    }
    // Make a list of keycodes to send. Rearrange them so that modifier keydowns always come first
    // and modifier keyups always come last. This ensures that shortcut keys will register properly
    // even if both keydown events happen at once.
    // + 2 is used because the volume wheel generates keydowns and keyups at the same time
    // + (-SCHAR_MIN) * 2 * 2 is used because a mouse wheel can generate up to 128*2 events at once, one for x, and one for y to be safe
    static_assert(sizeof(input->whl_rel_x) == sizeof(char) && sizeof(input->whl_rel_y) == sizeof(char),
        "Please change SCHAR_MIN below to the one for the appropriate type used by the scroll wheel.");
    int events[N_KEYS_INPUT + 2 + (-SCHAR_MIN) * 2 * 2];
    int modcount = 0, keycount = 0, rmodcount = 0;
    for(int byte = 0; byte < N_KEYBYTES_INPUT; byte++){
        char oldb = input->prevkeys[byte], newb = input->keys[byte];
        if(oldb == newb)
            continue;
        for(int bit = 0; bit < 8; bit++){
            int keyindex = byte * 8 + bit;
            if(keyindex >= N_KEYS_INPUT)
                break;
            const key* map = kb->keymap + keyindex;
            int scancode = (kb->active) ? bind->base[keyindex] : map->scan;
            char mask = 1 << bit;
            char old = oldb & mask, new = newb & mask;
            // If the key state changed, send it to the input device
            if(old != new){
                // Don't echo a key press if there's no scancode associated
                if(!(scancode & SCAN_SILENT)){
                    if(IS_MOD(scancode)){
                        // Only keyboards have modifiers, so ask explicitly for a sync if we only have a modifier in the queue
                        *sync_kb = 1;
                        if(new){
                            // Modifier down: Add to the end of modifier keys
                            for(int i = keycount + rmodcount; i > 0; i--)
                                events[modcount + i] = events[modcount + i - 1];
                            // Add 1 to the scancode because A is zero on OSX
                            // Positive code = keydown, negative code = keyup
                            events[modcount++] = scancode + 1;
                        } else {
                            // Modifier up: Add to the end of everything
                            events[modcount + keycount + rmodcount++] = -(scancode + 1);
                        }
                    } else if(IS_SCROLLWHEEL_V(scancode)) {
                        // Simulate scrolling if this came from a macro or a device without an axis (keyboard)
                        if(!input->whl_rel_y)
                            input->whl_rel_y = (scancode == BTN_WHEELUP ? 1 : -1);
                        os_mousescroll(kb, 0, input->whl_rel_y);
                        *sync_mouse = 1;
                    } else if(IS_SCROLLWHEEL_H(scancode)) {
                        // Simulate horizontal scrolling as no device supports this yet
                        // The only exception is binding the vertical scroll wheel to the horizontal one
                        if(IS_SCROLLWHEEL_V(map->scan))
                            input->whl_rel_x = input->whl_rel_y;
                        else
                            input->whl_rel_x = (scancode == BTN_WHEELLEFT ? -1 : 1);
                        os_mousescroll(kb, input->whl_rel_x, 0);
                        *sync_mouse = 1;
                    } else {
                        // Regular keypress: add to the end of regular keys
                        if(scancode & SCAN_MOUSE)
                            *sync_mouse = 1;
                        else
                            *sync_kb = 1;
                        memmove(events + modcount + keycount + 1, events + modcount + keycount, sizeof(int) * rmodcount);
                        events[modcount + keycount++] = new ? (scancode + 1) : -(scancode + 1);
                    }
                }

                // The volume/scroll wheels don't generate keyups, so create them automatically when necessary
                if(new){
                    if(scancode != KEY_UNBOUND){
                        // If it's a volume wheel, just add a single keyup event to the FIFO
                        // Else if the event originated from a scroll wheel, and is bound to something other than a scroll wheel,
                        // multiply the last few events by the amount of events received from the hid report
                        if(IS_VOLWHEEL(map->scan) || IS_CTRLWHEEL(kb, keyindex)){
                            // Make room for it
                            memmove(events + modcount + keycount + 1, events + modcount + keycount, sizeof(int) * rmodcount);
                            // Duplicate the last event, but with keyup
                            events[modcount + keycount++] = -(scancode + 1);
                        } else if (IS_SCROLLWHEEL_V(map->scan)){
                            if(!IS_SCROLLWHEEL_V(scancode)){
                                if(!input->whl_rel_y){
                                    ckb_err("whl_rel_y is 0");
                                    input->whl_rel_y = 1;
                                }
                                const int rel_y = abs(input->whl_rel_y);
                                // Make room for the extra events. We already have one, thus - 1.
                                memmove(events + modcount + keycount + rel_y * 2 - 1, events + modcount + keycount, sizeof(int) * rmodcount);
                                // Duplicate the last event, but with keyup
                                events[modcount + keycount++] = -(scancode + 1);

                                // Copy the last two events as many times as needed
                                for(int i = 0; i < rel_y - 1; i++){
                                    memcpy(events + modcount + keycount, events + modcount + keycount - 2, sizeof(int) * 2);
                                    keycount += 2;
                                }
                            }
                        }
                    }
                    // Clear the key bit so that we can receive events of this type in the next run
                    if(IS_VOLWHEEL(map->scan) || IS_SCROLLWHEEL_V(map->scan) || IS_CTRLWHEEL(kb, keyindex))
                        input->keys[byte] &= ~mask;
                }

                // Print notifications if desired
                if(kb->active){
                    for(int notify = 0; notify < OUTFIFO_MAX; notify++){
                        if(mode->notify[notify][byte] & mask){
                            // Wheels don't generate keyups and additionally can produce multiple events
                            // with a single report. No device so far supports scrolling horizontally,
                            // so only vertical notifications are implemented.
                            if(IS_SCROLLWHEEL_V(map->scan)){
                                for(int i = 0; i < abs(input->whl_rel_y); i++){
                                    nprintkey(kb, notify, keyindex, 1);
                                    nprintkey(kb, notify, keyindex, 0);
                                }
                            } else if(IS_VOLWHEEL(map->scan)){
                                    nprintkey(kb, notify, keyindex, 1);
                                    nprintkey(kb, notify, keyindex, 0);
                            } else {
                                nprintkey(kb, notify, keyindex, new);
                            }
                        }
                    }
                }
            }
        }
    }
    // Process all queued keypresses
    int totalkeys = modcount + keycount + rmodcount;
    int prev_scan = KEY_UNBOUND;
    for(int i = 0; i < totalkeys; i++){
        const int scancode = events[i];
        const int absscan = abs(scancode);
        // Force a keyboard sync if the event queue contains two of the same scancode before the second keypress is sent
        // This can't happen in mice because the wheel is an axis
        if(prev_scan == absscan){
#ifdef DEBUG_INPUT_SYNC
            ckb_info("Pre duplicate event sync");
#endif
            os_inputsync(kb, 1, 0);
#ifdef DEBUG_INPUT_SYNC
            ckb_info("Post duplicate event sync");
#endif
        }
        os_keypress(kb, absscan - 1, scancode > 0);
        prev_scan = absscan;
    }
}

void inputupdate(usbdevice* kb){
#ifdef OS_LINUX
    if((!kb->uinput_kb || !kb->uinput_mouse)
#else
    if(!kb->event
#endif
            || !kb->profile)
        return;

    usbinput* input = &kb->input;
    int sync_kb = 0;
    int sync_mouse = 0;

    // Let inputupdate_keys handle the wheel, while also keeping
    // the data reported by the mouse for the axis.
    // This is done to allow rebinding

    // Wheel up/down
    if(input->whl_rel_y > 0)
        SET_KEYBIT(input->keys, MOUSE_EXTRA_FIRST);
    else
        CLEAR_KEYBIT(input->keys, MOUSE_EXTRA_FIRST);

    if(input->whl_rel_y < 0)
        SET_KEYBIT(input->keys, MOUSE_EXTRA_FIRST + 1);
    else
        CLEAR_KEYBIT(input->keys, MOUSE_EXTRA_FIRST + 1);

    // Process key/button input
    inputupdate_keys(kb, &sync_kb, &sync_mouse);
    // Process mouse movement
    if(input->rel_x != 0 || input->rel_y != 0){
        os_mousemove(kb, input->rel_x, input->rel_y);
        input->rel_x = input->rel_y = 0;
        sync_mouse = 1;
    }
    // Finish up
    memcpy(input->prevkeys, input->keys, N_KEYBYTES_INPUT);
    input->whl_rel_x = input->whl_rel_y = 0;
    os_inputsync(kb, sync_kb, sync_mouse);
}

void updateindicators_kb(usbdevice* kb, int force){
    // Read current hardware indicator state (set externally)
    uchar old = kb->ileds, hw_old = kb->hw_ileds_old;
    uchar new = kb->hw_ileds, hw_new = new;
    // Update them if needed
    if(kb->active){
        usbmode* mode = kb->profile->currentmode;
        new = (new & ~mode->ioff) | mode->ion;
    }
    kb->ileds = new;
    kb->hw_ileds_old = hw_new;
    if(old != new || force){
        kb->vtable.delay(kb, DELAY_INDICATORS);

        ushort leds = kb->ileds;
        int len = 1;
        if((kb->protocol == PROTO_NXP && (kb->fwversion >= 0x300 || IS_V3_OVERRIDE(kb))) || kb->protocol == PROTO_BRAGI) {
            leds = (kb->ileds << 8) | 0x0001;
            len = 2;
        }
        queued_mutex_unlock(dmutex(kb));
        ctrltransfer transfer = { .bRequestType = 0x21, .bRequest = 0x09, .wValue = 0x0200, .wIndex = 0, .wLength = len, .timeout = 5000, .data = &leds };
        if(kb->protocol == PROTO_BRAGI)
            transfer.wValue++;
        os_usb_control(kb, &transfer, __FILE_NOPATH__, __LINE__);
        queued_mutex_lock(dmutex(kb));
    }
    // Print notifications if desired
    if(!kb->active)
        return;
    usbmode* mode = kb->profile->currentmode;
    uchar indicators[] = { I_NUM, I_CAPS, I_SCROLL };
    for(unsigned i = 0; i < sizeof(indicators) / sizeof(uchar); i++){
        uchar mask = indicators[i];
        if((hw_old & mask) == (hw_new & mask))
            continue;
        for(int notify = 0; notify < OUTFIFO_MAX; notify++){
            if(mode->inotify[notify] & mask)
                nprintind(kb, notify, mask, hw_new & mask);
        }
    }
}

///
/// \brief destroymacro free actions if macro is not currently running
/// \param macro
///
static inline void destroymacro(keymacro* macro) {
    if (macro->param){
        macro->param->removed = 1;
        abort_macro(macro->param);
    } else {
        free(macro->actions);
        free(macro);
    }
}

void initbind(binding* bind, usbdevice* kb){
    for(int i = 0; i < N_KEYS_INPUT; i++)
        bind->base[i] = kb->keymap[i].scan;
    bind->macros = calloc(32, sizeof(keymacro*));
    bind->macrocap = bind->macros ? 32 : 0;
    bind->macrocount = 0;
}

void freebind(binding* bind){
    for(int i = 0; i < bind->macrocount; i++)
        destroymacro(bind->macros[i]);
    free(bind->macros);
    memset(bind, 0, sizeof(*bind));
}

void cmd_bind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* to){
    (void)dummy;

    if(keyindex >= N_KEYS_INPUT)
        return;
    // Find the key to bind to
    uint tocode = 0;
    if(sscanf(to, "#x%ux", &tocode) != 1 && sscanf(to, "#%u", &tocode) == 1 && tocode < N_KEYS_INPUT){
        queued_mutex_lock(imutex(kb));
        mode->bind.base[keyindex] = tocode;
        queued_mutex_unlock(imutex(kb));
        return;
    }
    // If not numeric, look it up
    for(int i = 0; i < N_KEYS_INPUT; i++){
        if(kb->keymap[i].name && !strcmp(to, kb->keymap[i].name)){
            queued_mutex_lock(imutex(kb));
            mode->bind.base[keyindex] = kb->keymap[i].scan;
            queued_mutex_unlock(imutex(kb));
            return;
        }
    }
}

void cmd_unbind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* to){
    (void)dummy;
    (void)to;

    if(keyindex >= N_KEYS_INPUT)
        return;
    queued_mutex_lock(imutex(kb));
    mode->bind.base[keyindex] = KEY_UNBOUND;
    queued_mutex_unlock(imutex(kb));
}

void cmd_rebind(usbdevice* kb, usbmode* mode, int dummy, int keyindex, const char* to){
    (void)dummy;
    (void)to;

    if(keyindex >= N_KEYS_INPUT)
        return;
    queued_mutex_lock(imutex(kb));
    mode->bind.base[keyindex] = kb->keymap[keyindex].scan;
    queued_mutex_unlock(imutex(kb));
}

static void _cmd_macro(usbmode* mode, const char* keys, const char* assignment, usbdevice* kb){
    binding* bind = &mode->bind;
    if(!keys && !assignment){
        // Null strings = "macro clear" -> erase the whole thing
        for(int i = 0; i < bind->macrocount; i++)
            destroymacro(bind->macros[i]);
        bind->macrocount = 0;
        return;
    }
    if(bind->macrocount >= MACRO_MAX)
        return;
    // Create a key macro
    keymacro macro;
    memset(&macro, 0, sizeof(macro));
    // Scan the left side for key names, separated by +
    int empty = 1;
    int left = strlen(keys), right = strlen(assignment);
    int position = 0, field = 0;
    char keyname[N_KEYNAME_LENGTH];
    while(position < left && sscanf(keys + position, "%"N_SCANF_KEYNAME"[^+]%n", keyname, &field) == 1){
        // Find this key in the keymap
        for(unsigned i = 0; i < N_KEYS_INPUT; i++){
            if(kb->keymap[i].name && !strcmp(keyname, kb->keymap[i].name)){
                macro.combo[i / 8] |= 1 << (i % 8);
                empty = 0;
                break;
            }
        }
        if(keys[position += field] == '+')
            position++;
    }
    if(empty)
        return;
    // Count the number of actions (comma separated)
    int count = 1;
    for(const char* c = assignment; *c != 0; c++){
        if(*c == ',')
            count++;
    }
    // Allocate a buffer for them
    macro.actions = calloc(count, sizeof(macroaction));
    if(!macro.actions)
        return;
    macro.actioncount = 0;
    // Scan the actions
    position = 0;
    field = 0;

    // Check if repetition debounce and delay tokens are attached to the assignment.
    // Validate value to be between 0 and 2000 inclusive. Default to 500ms.
    const char *delay_tok = strchr(assignment, ':');
    macro.repetition_delay_ms = REPETITION_DEFAULT_VALUE;
    macro.repetition_debounce_ms = REPETITION_DEFAULT_VALUE;
    if (delay_tok != NULL) {
        static_assert(REPETITION_MAX_VALUE <= UINT32_MAX, "Repetition max value larger than what uint32_t can hold");
        uint32_t debounce_ms = 0;
        uint32_t delay_ms = 0;

        if (sscanf(delay_tok, ":%"SCNu32";%"SCNu32, &debounce_ms, &delay_ms) == 2) {
            if (debounce_ms > 0 && debounce_ms <= REPETITION_MAX_VALUE)
                macro.repetition_debounce_ms = (uint32_t) debounce_ms;
            if (delay_ms > 0 && delay_ms <= REPETITION_MAX_VALUE)
                macro.repetition_delay_ms = (uint32_t) delay_ms;
        }
    }

    // max action = old 11 chars plus 12 chars which is the max 32-bit unsigned int 4294967295 size * 2 + 1 for range underscore
    while(position < right && sscanf(assignment + position, "%"N_SCANF_KEYNAME"[^,:]%n", keyname, &field) == 1){
        if(!strcmp(keyname, "clear"))
            break;

        // Check for local key delay of the form '[+-]<key>=<delay>'
        int64_t long_delay;    // scanned delay value, used to keep delay in range.
        int64_t long_delay_range = 0;
        uint32_t delay = UINT32_MAX; // computed delay value. UINT32_MAX means use the default value.
        uint32_t delay_range = 0;
        char real_keyname[N_KEYNAME_LENGTH]; // temp to hold the left side (key) of the <key>=<delay>
        int scan_matches = sscanf(keyname, "%"N_SCANF_KEYNAME"[^=]=%"SCNd64"_%"SCNd64, real_keyname, &long_delay, &long_delay_range);
        if (scan_matches == 2 || scan_matches == 3) {
            if (0 <= long_delay && long_delay < UINT32_MAX) {
                delay = (uint32_t)long_delay;
                static_assert(sizeof(keyname) >= sizeof(real_keyname), "");
                strcpy(keyname, real_keyname);
                if(0 < long_delay_range && long_delay_range < UINT32_MAX
                        && long_delay_range > long_delay) {
                    delay_range = (uint32_t)long_delay_range;
                }
            }
        }

        int down = (keyname[0] == '+');
        if(down || keyname[0] == '-'){
            // Find this key in the keymap
            for(unsigned i = 0; i < N_KEYS_INPUT; i++){
                if(kb->keymap[i].name && !strcmp(keyname + 1, kb->keymap[i].name)){
                    macro.actions[macro.actioncount].scan = kb->keymap[i].scan;
                    macro.actions[macro.actioncount].down = down;
                    macro.actions[macro.actioncount].delay = delay;
                    macro.actions[macro.actioncount].delay_max = delay_range;
                    macro.actioncount++;
                    break;
                }
            }
        }
        if(assignment[position += field] == ',')
            position++;
    }

    keymacro* replacement = NULL;
    if(macro.actioncount){
        replacement = malloc(sizeof(*replacement));
        if(!replacement){
            free(macro.actions);
            return;
        }
        *replacement = macro;
    }
    // See if there's already a macro with this trigger
    keymacro** macros = bind->macros;
    for(int i = 0; i < bind->macrocount; i++){
        if(!memcmp(macros[i]->combo, macro.combo, N_KEYBYTES_INPUT)){
            destroymacro(macros[i]);
            // If the new macro has no actions, erase the existing one
            if(!macro.actioncount){
                for(int j = i + 1; j < bind->macrocount; j++)
                    macros[j - 1] = macros[j];
                bind->macrocount--;
            } else
                // If there are actions, replace the existing with the new
                macros[i] = replacement;
            return;
        }
    }

    // Add the macro to the device settings if not empty
    if(macro.actioncount < 1)
        return;
    if(bind->macrocount == bind->macrocap){
        int capacity = bind->macrocap + 16;
        keymacro** expanded = realloc(bind->macros, capacity * sizeof(*expanded));
        if(!expanded){
            destroymacro(replacement);
            return;
        }
        bind->macros = expanded;
        bind->macrocap = capacity;
    }
    bind->macros[bind->macrocount++] = replacement;
}

void cmd_macro(usbdevice* kb, usbmode* mode, const int notifynumber, const char* keys, const char* assignment){
    (void)notifynumber;

    queued_mutex_lock(imutex(kb));
    _cmd_macro(mode, keys, assignment, kb);
    queued_mutex_unlock(imutex(kb));
}
