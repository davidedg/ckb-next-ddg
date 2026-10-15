#include "cape_compile.h"
#include "cape_names.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* cape_compile_strerror(int err){
    switch(err){
    case CAPE_COMPILE_E_UNCOMPILABLE:
        return "a profile the files of the keyboard cannot hold";
    case CAPE_COMPILE_E_NOCELL:
        return "a light of another layout";
    default:
        return cape_profile_strerror(err);
    }
}

__attribute__((format(printf, 3, 4)))
static int fail(char* why, int rc, const char* fmt, ...){
    if(why){
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, CAPE_PROFILE_WHY, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static const char* str(const cape_profile_text* t){
    return t->p ? t->p : "";
}

static int key_of(const cape_profile_text* t){
    return cape_profile_key(t->p, t->len);
}

// Actions

// iCUE's Media actions, in the order of their enum (CAPE_PROFILE_MEDIA_*)
static const char* const media_names[] = { "PlayPause", "Stop", "PrevTrack", "NextTrack", "VolumeUp", "VolumeDown", "Mute" };
#define MEDIA_COUNT  (sizeof(media_names) / sizeof(media_names[0]))

// The events of a macro file, as iCUE's writer lays them out: a delay of 0 between two key events
typedef struct {
    cape_event* ev;
    unsigned n;          // may pass cap, which is then an internal error
    unsigned cap;
    int last_key;        // the last event is a key event
} events;

static int events_init(events* v, size_t cap){
    v->n = 0;
    v->cap = 0;
    v->last_key = 0;
    v->ev = calloc(cap ? cap : 1, sizeof(*v->ev));
    if(!v->ev)
        return CAPE_E_CAP;
    v->cap = (unsigned)cap;
    return CAPE_OK;
}

static int is_key_event(cape_event e){
    return !(e.b0 & 0x80);
}

static void put(events* v, cape_event e){
    if(v->n < v->cap)
        v->ev[v->n] = e;
    v->n++;
}

static void push(events* v, cape_event e){
    const int key = is_key_event(e);
    if(key && v->last_key){
        cape_event zero = { 0, 0, 0, 0 };
        cape_ev_delay(0, &zero);
        put(v, zero);
    }
    put(v, e);
    v->last_key = key;
}

// A delay as iCUE writes it, the longest being c0 ff ff ff
static cape_event delay_event(uint32_t ms){
    cape_event e = { 0, 0, 0, 0 };
    cape_ev_delay(ms > CAPE_DELAY_MAX ? CAPE_DELAY_MAX : ms, &e);
    return e;
}

// A key of a macro event: a key of the keyboard or a mouse button
static int event_key(const cape_profile_text* name, uint8_t* key, char* why){
    const int k = key_of(name);
    if(k == CAPE_PROFILE_NONE)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no key called \"%s\"", str(name));
    *key = (uint8_t)k;
    return CAPE_OK;
}

// The keys iCUE's Text types, with the US layout: the key and whether it is pressed with Shift; 0 for a character it leaves out
static int text_key(uint32_t cp, char name[2], const char** named, int* shift){
    static const char plain[] = "`-=[]\\;',./ \t\n";
    static const char* const plain_names[] = {
        "GraveAccentAndTilde", "MinusAndUnderscore", "EqualsAndPlus", "BracketLeft", "BracketRight", "Backslash", "SemicolonAndColon",
        "ApostropheAndDoubleQuote", "CommaAndLessThan", "PeriodAndBiggerThan", "SlashAndQuestionMark", "Space", "Tab", "Enter",
    };
    static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    static const char* const shifted_names[] = {
        "GraveAccentAndTilde", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "MinusAndUnderscore", "EqualsAndPlus", "BracketLeft",
        "BracketRight", "Backslash", "SemicolonAndColon", "ApostropheAndDoubleQuote", "CommaAndLessThan", "PeriodAndBiggerThan",
        "SlashAndQuestionMark",
    };
    *named = NULL;
    *shift = 0;
    if((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')){
        name[0] = (char)(cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp);
        name[1] = 0;
        *named = name;
        *shift = cp >= 'A' && cp <= 'Z';
        return 1;
    }
    if(cp == 0 || cp > 0x7f)
        return 0;
    const char* at = strchr(plain, (int)cp);
    if(at){
        *named = plain_names[at - plain];
        return 1;
    }
    at = strchr(shifted, (int)cp);
    if(at){
        *named = shifted_names[at - shifted];
        *shift = 1;
        return 1;
    }
    return 0;
}

// The next code point of a UTF-8 string, or -1 for a malformed one
static long utf8_next(const uint8_t* s, size_t len, size_t* i){
    const uint8_t c = s[*i];
    unsigned n;
    uint32_t cp;
    if(c < 0x80){
        (*i)++;
        return c;
    } else if(c >= 0xc2 && c <= 0xdf){
        n = 1;
        cp = c & 0x1f;
    } else if(c >= 0xe0 && c <= 0xef){
        n = 2;
        cp = c & 0x0f;
    } else if(c >= 0xf0 && c <= 0xf4){
        n = 3;
        cp = c & 0x07;
    } else
        return -1;
    if(len - *i <= n)
        return -1;
    for(unsigned k = 1; k <= n; k++){
        const uint8_t d = s[*i + k];
        if((d & 0xc0) != 0x80)
            return -1;
        cp = (cp << 6) | (d & 0x3f);
    }
    if((n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)))
        return -1;
    *i += n + 1;
    return (long)cp;
}

// iCUE's limits of a Text: 1024 characters as its field counts them (UTF-16 units), 99999 ms between them
#define TEXT_CHARS_MAX  1024

static int text_events(const cape_profile_action* a, events* v, char* why){
    const uint8_t* s = (const uint8_t*)str(&a->text);
    const size_t len = a->text.len;
    size_t units = 0;
    for(size_t i = 0; i < len;){
        const long cp = utf8_next(s, len, &i);
        if(cp < 0)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a Text that is not UTF-8");
        units += cp >= 0x10000 ? 2 : 1;
    }
    if(units > TEXT_CHARS_MAX || a->char_delay > CAPE_BIND_TEXT_DELAY_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a Text of %zu characters, %u ms between them: iCUE's limits are %d and %d", units,
                    (unsigned)a->char_delay, TEXT_CHARS_MAX, CAPE_BIND_TEXT_DELAY_MAX);
    // Each character: up to four key events, three delays of 0 between them and the delay before it
    int rc = events_init(v, 8 * len);
    if(rc != CAPE_OK)
        return rc;
    const uint8_t lshift = (uint8_t)cape_profile_key("LeftShift", 9);
    unsigned typed = 0;
    for(size_t i = 0; i < len;){
        const long cp = utf8_next(s, len, &i);
        char letter[2];
        const char* named;
        int shift;
        if(!text_key((uint32_t)cp, letter, &named, &shift))
            continue;
        const int k = cape_profile_key(named, strlen(named));
        if(k == CAPE_PROFILE_NONE)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no key called \"%s\"", named);
        if(typed++)
            push(v, delay_event(a->char_delay));
        if(shift)
            push(v, cape_ev_press(lshift));
        push(v, cape_ev_press((uint8_t)k));
        push(v, cape_ev_release((uint8_t)k));
        if(shift)
            push(v, cape_ev_release(lshift));
    }
    return CAPE_OK;
}

// The order of the keys of a shortcut: Win, Ctrl, Alt, Shift, then the others
static int modifier_rank(const cape_profile_text* t){
    static const char* const order[] = { "LeftGui", "LeftCtrl", "LeftAlt", "LeftShift" };
    for(int i = 0; i < 4; i++)
        if(t->len == strlen(order[i]) && !memcmp(str(t), order[i], t->len))
            return i;
    return 4;
}

static int shortcut_events(const cape_profile_action* a, events* v, char* why){
    if(a->nstroke > CAPE_BIND_EDIT_EVENTS_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a shortcut of %u keys", (unsigned)a->nstroke);
    uint8_t* keys = malloc(a->nstroke ? a->nstroke : 1);
    int* rank = malloc((a->nstroke ? a->nstroke : 1) * sizeof(*rank));
    int rc = keys && rank ? events_init(v, 4 * (size_t)a->nstroke) : CAPE_E_CAP;
    for(uint32_t i = 0; rc == CAPE_OK && i < a->nstroke; i++){
        rc = event_key(&a->stroke[i], &keys[i], why);
        rank[i] = modifier_rank(&a->stroke[i]);
    }
    if(rc == CAPE_OK){
        // A stable sort by rank
        for(uint32_t i = 1; i < a->nstroke; i++){
            const uint8_t k = keys[i];
            const int r = rank[i];
            uint32_t j = i;
            for(; j > 0 && rank[j - 1] > r; j--){
                keys[j] = keys[j - 1];
                rank[j] = rank[j - 1];
            }
            keys[j] = k;
            rank[j] = r;
        }
        for(uint32_t i = 0; i < a->nstroke; i++)
            push(v, cape_ev_press(keys[i]));
        for(uint32_t i = a->nstroke; i > 0; i--)
            push(v, cape_ev_release(keys[i - 1]));
    }
    free(keys);
    free(rank);
    return rc;
}

static int macro_events(const cape_profile_action* a, events* v, char* why){
    if(a->termination != CAPE_PROFILE_TERMINATE_INTERRUPT || a->high_priority)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a Macro that is not interrupted, or of high priority");
    if(a->nevents > CAPE_BIND_MACRO_ROWS_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a Macro of %u events: iCUE records at most %d", (unsigned)a->nevents,
                    CAPE_BIND_MACRO_ROWS_MAX);
    // A key stroke is a press and a release, with a delay of 0 before each
    int rc = events_init(v, 4 * (size_t)a->nevents);
    for(uint32_t i = 0; rc == CAPE_OK && i < a->nevents; i++){
        const cape_profile_event* e = &a->events[i];
        if(e->kind == CAPE_PROFILE_DELAY_EVENT){
            cape_event d = { 0, 0, 0, 0 };
            if(e->random){
                if(e->delay_min > e->delay_max || e->delay_max > CAPE_BIND_MACRO_DELAY_MAX)
                    return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a random delay from %u to %u ms", (unsigned)e->delay_min,
                                (unsigned)e->delay_max);
                cape_ev_random(e->delay_min, e->delay_max, &d);
            } else {
                if(e->delay > CAPE_BIND_MACRO_DELAY_MAX)
                    return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a delay of %u ms in a Macro: iCUE records at most %d",
                                (unsigned)e->delay, CAPE_BIND_MACRO_DELAY_MAX);
                cape_ev_delay(e->delay, &d);
            }
            push(v, d);
            continue;
        }
        if(e->nkeys != 1)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a key event of a Macro with %u keys", (unsigned)e->nkeys);
        uint8_t k;
        rc = event_key(&e->keys[0], &k, why);
        if(rc != CAPE_OK)
            break;
        if(e->sub == CAPE_PROFILE_SUB_PRESS || e->sub == CAPE_PROFILE_SUB_STROKE)
            push(v, cape_ev_press(k));
        if(e->sub == CAPE_PROFILE_SUB_RELEASE || e->sub == CAPE_PROFILE_SUB_STROKE)
            push(v, cape_ev_release(k));
    }
    return rc;
}

int cape_compile_action(const cape_profile_action* a, cape_binding_action* out, char* why){
    if(!a || !out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(a->sound_path.len || memcmp(a->lighting.b, (const uint8_t[16]){ 0 }, 16))
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action with a sound or a lighting");
    if(a->repeat_mode != CAPE_PROFILE_REPEAT_NO || a->repeat_count != 1 || a->repeat_delay || a->random_delay_from
       || a->random_delay_to || a->delay_mode != CAPE_PROFILE_DELAY_CONSTANT)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action that repeats");
    if(a->retain_original || a->terminate_on_second)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action that keeps the key's own output or ends on a second press");
    uint8_t start = 0, run = 1, repeat = 1;
    switch(a->exec_hint){
    case CAPE_PROFILE_HINT_ON_BOTH:
    case CAPE_PROFILE_HINT_ON_PRESS:
        break;
    case CAPE_PROFILE_HINT_ON_RELEASE:
        start = 0x11;
        break;
    case CAPE_PROFILE_HINT_WHILE_PRESSED:
        run = 3;
        break;
    case CAPE_PROFILE_HINT_TOGGLE:
        run = 4;
        break;
    default:
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action on a long press");
    }

    events v = { NULL, 0, 0, 0 };
    uint8_t subtype = 0;
    int rc = CAPE_OK;
    switch(a->kind){
    case CAPE_PROFILE_MEDIA: {
        const int k = a->media < MEDIA_COUNT ? cape_profile_media(media_names[a->media], strlen(media_names[a->media])) : CAPE_PROFILE_NONE;
        if(k == CAPE_PROFILE_NONE)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a Media action this keyboard has no key for");
        out->kind = CAPE_BIND_REMAP;
        out->dst = (uint8_t)k;
        return CAPE_OK;
    }
    case CAPE_PROFILE_REMAP:
        if(a->program_path.len)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a remap that starts a program");
        if(a->holding_enabled){
            if((a->holding_type != CAPE_PROFILE_HOLD_ON_PRESS && a->holding_type != CAPE_PROFILE_HOLD_TOGGLE) || !a->key_name.len || a->nstroke)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an 'Imitate holding key' while pressed, or not of one key");
            uint8_t k;
            if(event_key(&a->key_name, &k, why) != CAPE_OK)
                return CAPE_COMPILE_E_UNCOMPILABLE;
            if(a->holding_type == CAPE_PROFILE_HOLD_TOGGLE){
                run = 0x84;
                repeat = 0;
            }
            rc = events_init(&v, 3);
            if(rc == CAPE_OK){
                push(&v, cape_ev_press(k));
                push(&v, delay_event(a->holding_type == CAPE_PROFILE_HOLD_ON_PRESS ? a->holding_interval : 0));
                push(&v, cape_ev_release(k));
            }
            start = 0;
            subtype = CAPE_MACRO_SHORTCUT;
            break;
        }
        if(a->key_name.len && !a->nstroke){
            const int k = key_of(&a->key_name);
            if(k == CAPE_PROFILE_NONE)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no key called \"%s\"", str(&a->key_name));
            out->kind = CAPE_BIND_REMAP;
            out->dst = (uint8_t)k;
            return CAPE_OK;
        }
        if(!a->key_name.len && !a->nstroke){
            out->kind = CAPE_BIND_NATIVE;
            return CAPE_OK;
        }
        if(a->key_name.len)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a remap with both a key and a shortcut");
        rc = shortcut_events(a, &v, why);
        subtype = CAPE_MACRO_SHORTCUT;
        break;
    case CAPE_PROFILE_TEXT:
        rc = text_events(a, &v, why);
        subtype = CAPE_MACRO_TEXT;
        break;
    case CAPE_PROFILE_MACRO:
        rc = macro_events(a, &v, why);
        subtype = CAPE_MACRO_MACRO;
        break;
    default:
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action of an unknown kind");
    }
    if(rc == CAPE_OK && v.n > v.cap)
        rc = fail(why, CAPE_E_CAP, "internal: more events than room");
    if(rc == CAPE_OK && v.n > CAPE_BIND_EDIT_EVENTS_MAX)
        rc = fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a macro file of %u events: the most iCUE writes is %d", v.n,
                  CAPE_BIND_EDIT_EVENTS_MAX);
    if(rc != CAPE_OK){
        free(v.ev);
        return rc;
    }
    out->kind = CAPE_BIND_MACRO;
    out->subtype = subtype;
    out->start = start;
    out->run = run;
    out->repeat = repeat;
    out->events = v.ev;
    out->nevents = v.n;
    return CAPE_OK;
}

// Bindings

int cape_compile_bindings(const cape_profile_profile* p, cape_binding_model* out, char* why){
    if(!out)
        return CAPE_E_ARG;
    cape_binding_init(out);
    if(!p)
        return CAPE_E_ARG;
    if(p->disable_encryption || p->disable_touchpad || !p->swap_fkeys)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "Win Lock options of another device");
    if(p->preset_active || memcmp(p->active_preset.b, (const uint8_t[16]){ 0 }, 16) || p->hw_actions_types)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "presets of actions");
    out->winlock = (uint8_t)((p->disable_win ? CAPE_WINLOCK_WINKEY : 0) | (p->disable_alt_tab ? CAPE_WINLOCK_ALT_TAB : 0)
                             | (p->disable_alt_f4 ? CAPE_WINLOCK_ALT_F4 : 0) | (p->disable_shift_tab ? CAPE_WINLOCK_SHIFT_TAB : 0));
    uint8_t taken[CAPE_BIND_K95_KEYS] = { 0 };
    for(uint32_t i = 0; i < p->nbindings; i++){
        const cape_profile_binding* b = &p->bindings[i];
        if(b->layer != CAPE_PROFILE_LAYER_STANDARD || b->event != CAPE_PROFILE_EVENT_CLICK || b->distance)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action of \"%s\" on another layer or a long press", str(&b->key));
        // an action on no key: iCUE writes nothing of it in the files
        if(!b->key.len)
            continue;
        const int key = key_of(&b->key);
        if(key < 0 || key >= CAPE_BIND_K95_KEYS)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action on \"%s\", not a key of this keyboard", str(&b->key));
        if(taken[key]++)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "two actions on the key of \"%s\"", str(&b->key));
        cape_binding_action a;
        char reason[CAPE_PROFILE_WHY];
        int rc = cape_compile_action(&b->action, &a, reason);
        if(rc != CAPE_OK){
            if(rc == CAPE_COMPILE_E_UNCOMPILABLE)
                fail(why, rc, "the action of %s: %s", str(&b->key), reason);
            return rc;
        }
        if(a.kind == CAPE_BIND_REMAP)
            rc = cape_binding_set_remap(out, (unsigned)key, a.dst);
        else if(a.kind == CAPE_BIND_MACRO)
            rc = cape_binding_set_macro(out, (unsigned)key, a.subtype, a.start, a.run, a.repeat, a.events, a.nevents);
        free(a.events);
        if(rc != CAPE_OK)
            return rc == CAPE_E_CAP ? CAPE_E_CAP : fail(why, rc, "internal: the action of %s not set", str(&b->key));
    }
    return CAPE_OK;
}

// Indicators

static int component(double c){
    if(!(c >= 0))
        return 0;
    return c > 1 ? 255 : (int)(c * 255 + 0.5);
}

static void rgb_of(const cape_profile_colour* c, uint8_t out[3]){
    for(int i = 0; i < 3; i++)
        out[i] = (uint8_t)component(c->c[i]);
}

static void optional_rgb(const cape_profile_optcolour* c, uint32_t def, uint8_t out[3]){
    if(c->present)
        rgb_of(&c->colour, out);
    else {
        out[0] = (uint8_t)(def >> 16);
        out[1] = (uint8_t)(def >> 8);
        out[2] = (uint8_t)def;
    }
}

int cape_compile_indicators(const cape_profile_profile* p, uint8_t out[CAPE_COMPILE_IND_SIZE]){
    if(!p || !out)
        return CAPE_E_ARG;
    optional_rgb(&p->profile, 0xff0000, out);
    rgb_of(&p->brightness, out + 3);
    optional_rgb(&p->winlock_on, 0x00ffff, out + 6);
    optional_rgb(&p->winlock_off, 0xff0000, out + 9);
    return CAPE_OK;
}

// Lighting

static int is_predefined(cape_profile_effect_kind k){
    return k != CAPE_PROFILE_STATIC && k != CAPE_PROFILE_SOLID && k != CAPE_PROFILE_GRADIENT && k != CAPE_PROFILE_WAVE && k != CAPE_PROFILE_RIPPLE;
}

// The cells of a layer on a layout, in iCUE's order
static int layer_cells(const cape_profile_layer* l, int layout, uint8_t cells[CAPE_PROFILE_LAYER_MAX], unsigned* n, char* why){
    if(l->option == CAPE_PROFILE_OPTION_ALL){
        *n = cape_profile_layout_all(layout, cells, NULL);
        return CAPE_OK;
    }
    if(l->nkeys && !l->keys) return CAPE_E_ARG;
    // by their place in the order (fewer than 256 lights)
    int cell_at[256];
    for(int i = 0; i < 256; i++)
        cell_at[i] = -1;
    for(uint32_t i = 0; i < l->nkeys; i++){
        const cape_profile_text* name = &l->keys[i];
        int order = 0;
        const int cell = cape_profile_light(name->p, name->len, layout, &order);
        if(cell == CAPE_PROFILE_NONE){
            const int led = cape_profile_led(name->p, name->len);
            if(led == CAPE_LED_PROFILE || led == CAPE_LED_BRIGHTNESS || led == CAPE_LED_WINLOCK)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "the light \"%s\" in a layer: its colour is an indicator's", str(name));
            for(int other = CAPE_PROFILE_LAYOUT_ANSI; other <= CAPE_PROFILE_LAYOUT_KS; other++)
                if(cape_profile_light(name->p, name->len, other, NULL) != CAPE_PROFILE_NONE)
                    return fail(why, CAPE_COMPILE_E_NOCELL, "the light \"%s\" is not one of layout %d", str(name), layout);
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no light called \"%s\"", str(name));
        }
        if(order < 0 || order > 255)
            return fail(why, CAPE_E_ARG, "internal: the place of \"%s\"", str(name));
        if(cell_at[order] >= 0)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "the light \"%s\" twice in a layer", str(name));
        cell_at[order] = cell;
    }
    *n = 0;
    for(int i = 0; i < 256 && *n < CAPE_PROFILE_LAYER_MAX; i++)
        if(cell_at[i] >= 0)
            cells[(*n)++] = (uint8_t)cell_at[i];
    return CAPE_OK;
}

static int set_file(cape_light_file* f, unsigned layer, char ext, int len){
    if(len < 0)
        return len;
    char name[10];
    cape_light_filename(layer, ext, name);
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->len = (size_t)len;
    return CAPE_OK;
}

// Animation geometry uses LED anchors, not the rectangles drawn by the GUI.
static int animation_origin(const cape_profile_layer* l, int layout, int32_t* ox, int32_t* oy){
    int ax[CAPE_PROFILE_LAYER_MAX + 3], ay[CAPE_PROFILE_LAYER_MAX + 3];
    const unsigned all = cape_profile_anchors(layout, ax, ay);
    if(!all) return 0;
    int rx = ax[0], ry = ay[0];
    for(unsigned i = 1; i < all; ++i){
        if(ax[i] < rx) rx = ax[i];
        if(ay[i] < ry) ry = ay[i];
    }
    const unsigned n = l->option == CAPE_PROFILE_OPTION_ALL ? all : l->nkeys;
    if(n > CAPE_PROFILE_LAYER_MAX + 3) return 0;
    int left = 0, top = 0, right = -1, bottom = -1, sx = 0, sy = 0;
    for(unsigned i = 0; i < n; ++i){
        int x, y;
        if(l->option == CAPE_PROFILE_OPTION_ALL){ x = ax[i]; y = ay[i]; }
        else if(!cape_profile_anchor(l->keys[i].p, l->keys[i].len, layout, &x, &y)) return 0;
        // QRectF(anchor, zero size).toRect().center(), including signed integer division.
        x = (2 * x - 1) / 2; y = (2 * y - 1) / 2;
        if(!i){ left = right = x; top = bottom = y; }
        if(x < left) left = x;
        if(x > right) right = x;
        if(y < top) top = y;
        if(y > bottom) bottom = y;
        sx += x; sy += y;
    }
    int x = 0, y = 0;
    if(l->effect.kind == CAPE_PROFILE_RIPPLE){
        if(n){ x = (int)round((double)sx / n); y = (int)round((double)sy / n); }
    } else if(l->effect.double_sided){
        x = (left + right) / 2; y = (top + bottom) / 2;
    } else {
        const int angle = (int)l->effect.angle;
        if(angle == 0){ x = left; y = (top + bottom) / 2; }
        else if(angle < 90){ x = left; y = bottom; }
        else if(angle == 90){ x = (left + right) / 2; y = bottom; }
        else if(angle < 180){ x = right; y = bottom; }
        else if(angle == 180){ x = right; y = bottom / 2 + top; }
        else if(angle < 270){ x = right; y = top; }
        else if(angle == 270){ x = (left + right) / 2; y = top; }
        else { x = left; y = top; }
    }
    *ox = (int32_t)((x - rx) * 0.302);
    *oy = (int32_t)((y - ry) * 0.2015);
    return 1;
}

static void animation_samples(const cape_profile_effect* e, cape_rgba samples[CAPE_LIGHT_SAMPLES]){
    memset(samples, 0, sizeof(cape_rgba) * CAPE_LIGHT_SAMPLES);
    if(e->ntransitions < 2) return;
    for(unsigned j = 0; j < CAPE_LIGHT_SAMPLES; ++j){
        const int mark = j ? (int)((j - 1) * 1000u / 127u) : 0;
        uint32_t k = 0;
        while(k < e->ntransitions && (int)round(e->transitions[k].time * 1000.0) <= mark) ++k;
        if(!k || k == e->ntransitions) continue;
        const cape_profile_transition* a = &e->transitions[k - 1];
        const cape_profile_transition* b = &e->transitions[k];
        const int t0 = (int)round(a->time * 1000.0), t1 = (int)round(b->time * 1000.0);
        uint8_t colour[4];
        for(unsigned c = 0; c < 4; ++c){
            const int ca = c == 3 && !a->colour.has_alpha ? 255 : component(a->colour.c[c]);
            const int cb = c == 3 && !b->colour.has_alpha ? 255 : component(b->colour.c[c]);
            int v;
            if(e->kind == CAPE_PROFILE_SOLID) v = (k - 1) % 2 ? 0 : ca;
            else v = (int)(ca * ((double)(t1 - mark) / (t1 - t0))) + (int)(cb * ((double)(mark - t0) / (t1 - t0)));
            colour[c] = (uint8_t)(v > 255 ? 255 : v);
        }
        samples[j] = (cape_rgba){colour[0], colour[1], colour[2], colour[3]};
    }
}

static int animation_descriptor(const cape_profile_layer* l, int layout, uint8_t* out, size_t cap, char* why){
    const cape_profile_effect* e = &l->effect;
    cape_light_custom d = {0};
    d.effect = e->kind == CAPE_PROFILE_SOLID ? CAPE_FX_ADVANCEDSOLID : e->kind == CAPE_PROFILE_GRADIENT ? CAPE_FX_ADVANCEDGRADIENT :
               e->kind == CAPE_PROFILE_RIPPLE ? CAPE_FX_ADVANCEDRIPPLE : CAPE_FX_ADVANCEDWAVE;
    if(round(e->duration / 100.0) > UINT16_MAX || l->stop_after_times > UINT16_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "animation duration or repeat count exceeds its firmware field");
    d.duration_100ms = (uint16_t)round(e->duration / 100.0);
    d.stop_after_times = l->stop_after_times < 0 ? 0 : (uint16_t)l->stop_after_times;
    d.start_on_key_press = l->start_on_key_press;
    d.start_with_profile = l->start_with_profile;
    d.stop_on_key_press = l->stop_on_key_press;
    d.stop_on_key_release = l->stop_on_key_release;
    if(e->kind == CAPE_PROFILE_WAVE || e->kind == CAPE_PROFILE_RIPPLE){
        if(!isfinite(e->tail_length) || !isfinite(e->velocity) || e->tail_length < 0 || e->velocity < 0 ||
           trunc(e->tail_length) != e->tail_length || trunc(e->velocity) != e->velocity ||
           round(e->tail_length / 10.0) > UINT8_MAX || round(e->velocity / 10.0) > UINT8_MAX)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "animation tail or velocity has no qualified firmware representation");
        d.tail = (uint8_t)round(e->tail_length / 10.0);
        d.velocity = (uint8_t)round(e->velocity / 10.0);
        d.unknown4 = 1;
        if(e->kind == CAPE_PROFILE_WAVE){
            if(!isfinite(e->angle) || trunc(e->angle) != e->angle || e->angle < 0 || e->angle > 359)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "animation angle is not an integer in 0..359");
            d.angle = (uint32_t)e->angle;
            d.two_sided = e->double_sided;
        } else {
            static const uint8_t spread[] = {1, 3, 5, 7};
            if(e->wave_spread >= sizeof(spread)) return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "unknown ripple spread");
            d.unknown4 = spread[e->wave_spread];
        }
        if(!animation_origin(l, layout, &d.origin_x, &d.origin_y))
            return fail(why, CAPE_COMPILE_E_NOCELL, "animation origin has an unknown light or layout");
    }
    animation_samples(e, d.samples);
    return cape_light_custom_build(&d, out, cap);
}

static int preset_descriptor(const cape_profile_effect* e, uint8_t* out, size_t cap, char* why){
    static const uint8_t type[CAPE_PROFILE_EFFECTS] = {7, 10, 11, 13, 12, 1, 0, 4, 6, 3, 2, 8, 5};
    if(cap < CAPE_LIGHT_DESC_PREDEFINED) return CAPE_E_CAP;
    memset(out, 0, CAPE_LIGHT_DESC_PREDEFINED);
    out[0] = type[e->kind];
    if(e->kind == CAPE_PROFILE_TYPE && e->type_behaviour > 1)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "unknown type-lighting behaviour");
    if(e->kind == CAPE_PROFILE_TYPE && !e->type_behaviour){
        if(e->type_duration < 1 || e->type_duration > 3)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "type-lighting duration is not in 1..3");
        out[4] = (uint8_t)e->type_duration;
    } else {
        if(e->speed < 1 || e->speed > 3) return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "preset speed is not in 1..3");
        out[1] = (uint8_t)e->speed;
        if(e->kind == CAPE_PROFILE_TYPE) out[0] = 9;
    }
    if(e->kind == CAPE_PROFILE_RAINBOW_WAVE || e->kind == CAPE_PROFILE_COLOR_WAVE){
        if(e->direction < 1 || e->direction > 4) return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "preset direction is not in 1..4");
        out[3] = (uint8_t)e->direction;
    } else if(e->kind == CAPE_PROFILE_SPIRAL_RAINBOW) out[3] = e->clockwise ? 5 : 6;
    if(e->kind != CAPE_PROFILE_RAINBOW_WAVE && e->kind != CAPE_PROFILE_SPIRAL_RAINBOW){
        out[2] = e->random ? 1 : 3;
        if(!e->random)
            for(unsigned i = 0; i < 2; ++i){
                const uint32_t c = e->colours[i];
                out[5 + i * 4] = (uint8_t)(c >> 16); out[6 + i * 4] = (uint8_t)(c >> 8);
                out[7 + i * 4] = (uint8_t)c; out[8 + i * 4] = (uint8_t)(c >> 24);
            }
    }
    return CAPE_LIGHT_DESC_PREDEFINED;
}

int cape_compile_lighting_build(const cape_profile_profile* p, int layout, cape_compile_lighting* out, char* why){
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!p)
        return CAPE_E_ARG;
    if(p->nlayers && !p->layers) return CAPE_E_ARG;
    if(!p->side_lighting)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "the side lighting turned off");
    if(p->nlayers > CAPE_LAYERS_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "%u layers: the keyboard holds %d", (unsigned)p->nlayers, CAPE_LAYERS_MAX);
    unsigned all_colours = 0;
    uint8_t all[CAPE_PROFILE_LAYER_MAX];
    if(p->nlayers && !cape_profile_layout_all(layout, all, &all_colours))
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "lighting layers on an unknown layout (%d)", layout);
    // Limits apply to the whole ZIP model, including layers that are turned off.
    for(uint32_t i = 0; i < p->nlayers; i++){
        const char* error = cape_profile_layer_error(&p->layers[i]);
        if(error) return fail(why, CAPE_PROFILE_E_CORRUPT, "%s", error);
        if((unsigned)p->layers[i].effect.kind >= CAPE_PROFILE_EFFECTS)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "unknown lighting type");
        if(is_predefined(p->layers[i].effect.kind) && p->nlayers != 1)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a predefined effect with other layers");
    }
    // From the bottom of iCUE's list, only enabled layers get compact firmware indices.
    for(uint32_t remaining = p->nlayers; remaining > 0; remaining--){
        const cape_profile_layer* l = &p->layers[remaining - 1];
        if(!l->enabled)
            continue;
        const unsigned f = out->layers++;
        const cape_profile_effect* e = &l->effect;
        if(l->play_option != CAPE_PROFILE_PLAY_STANDARD)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "layer %u played from a key", f);
        if(e->brightness != 10)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "layer %u at another brightness", f);
        if(is_predefined(e->kind)){
            if(p->nlayers != 1 || l->option != CAPE_PROFILE_OPTION_ALL)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a predefined effect with other layers, or not on all keys");
            if(!l->start_with_profile || l->start_on_key_press || l->stop_on_key_press || l->stop_on_key_release
               || l->stop_after_times != -1)
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a predefined effect that starts or stops on a key");
            out->effects = 1;
            out->count++;
            out->bytes += CAPE_LIGHT_DESC_PREDEFINED;
            cape_light_file* file = &out->file[out->nfiles];
            const int rc = set_file(file, f, 'd', preset_descriptor(e, file->data, sizeof(file->data), why));
            if(rc != CAPE_OK) return rc;
            ++out->nfiles;
            continue;
        }
        uint8_t cells[CAPE_PROFILE_LAYER_MAX];
        unsigned n = 0;
        int rc = layer_cells(l, layout, cells, &n, why);
        if(rc != CAPE_OK)
            return rc;
        if(e->ntransitions && !e->transitions) return CAPE_E_ARG;
        for(uint32_t t = 0; t < e->ntransitions; t++){
            for(unsigned c = 0; c < (e->transitions[t].colour.has_alpha ? 4u : 3u); ++c)
                if(!isfinite(e->transitions[t].colour.c[c]) || e->transitions[t].colour.c[c] < 0 || e->transitions[t].colour.c[c] > 1)
                    return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "invalid animation colour");
            if(!(e->transitions[t].time >= 0 && e->transitions[t].time <= 1) || (t && e->transitions[t].time < e->transitions[t - 1].time))
                return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "layer %u: its colours are not in order of time", f);
        }
        if(e->kind != CAPE_PROFILE_STATIC){
            out->effects = 1;
            out->count += 2;
            out->bytes += CAPE_LIGHT_DESC_CUSTOM + 4 + n;
            cape_light_file* file = &out->file[out->nfiles];
            rc = set_file(&file[0], f, 'd', animation_descriptor(l, layout, file[0].data, sizeof(file[0].data), why));
            if(rc == CAPE_OK) rc = set_file(&file[1], f, 'k', cape_keylist_build(cells, n, file[1].data, sizeof(file[1].data)));
            if(rc != CAPE_OK) return rc;
            out->nfiles += 2;
            continue;
        }
        if(e->ntransitions != 1)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a static layer of %u colours", (unsigned)e->ntransitions);
        if(!l->start_with_profile || l->start_on_key_press || l->stop_on_key_press || l->stop_on_key_release
           || (l->stop_after_times != -1 && l->stop_after_times != 0))
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a static layer that starts or stops on a key");
        if(e->opacity > 255)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a static layer of opacity %u", (unsigned)e->opacity);
        out->count += 3;
        out->bytes += CAPE_LIGHT_DESC_STATIC + 4 + n + CAPE_COLORS_SIZE;
        // The files: the descriptor, the cells, a colour for each (for all keys, as many as iCUE writes: the three buttons too)
        cape_light_file* file = &out->file[out->nfiles];
        cape_light_desc d;
        cape_light_desc_static(&d);
        rc = set_file(&file[0], f, 'd', cape_light_desc_build(&d, file[0].data, sizeof(file[0].data)));
        if(rc == CAPE_OK)
            rc = set_file(&file[1], f, 'k', cape_keylist_build(cells, n, file[1].data, sizeof(file[1].data)));
        if(rc == CAPE_OK){
            cape_rgba colours[CAPE_COLORS_COUNT];
            memset(colours, 0, sizeof(colours));
            uint8_t rgb[3];
            rgb_of(&e->transitions[0].colour, rgb);
            const unsigned ncolours = l->option == CAPE_PROFILE_OPTION_ALL ? all_colours : n;
            for(unsigned i = 0; i < ncolours; i++)
                colours[i] = (cape_rgba){ rgb[0], rgb[1], rgb[2], (uint8_t)e->opacity };
            rc = set_file(&file[2], f, 'r', cape_colors_build(colours, file[2].data, sizeof(file[2].data)));
        }
        if(rc != CAPE_OK)
            return fail(why, rc, "internal: the files of layer %u not made", f);
        out->nfiles += 3;
    }
    out->count++;
    out->bytes += CAPE_LAYERCOUNT_SIZE;
    cape_light_file* cnt = &out->file[out->nfiles];
    snprintf(cnt->name, sizeof(cnt->name), "%s", CAPE_FILE_LAYERCOUNT);
    const int len = cape_layercount_build(out->layers, cnt->data, sizeof(cnt->data));
    if(len < 0)
        return fail(why, len, "internal: lghtcnt.cnt not made");
    cnt->len = (size_t)len;
    out->nfiles++;
    return CAPE_OK;
}

// Importing

// What a new profile of iCUE has where the files say nothing: the colours of the M1 to M3 buttons
static const cape_profile_colour new_m1 = { { 1, 0.47451, 0, 1 }, 0 };
static const cape_profile_colour new_m2 = { { 0.686275, 0.0980392, 0, 1 }, 0 };
static const cape_profile_colour new_m3 = { { 0.686275, 0, 0.6, 1 }, 0 };

// The tabs of iCUE's Remap an action was chosen in (remapGroup)
enum { GROUP_KEYBOARD = 0, GROUP_MOUSE = 6, GROUP_KEYSTROKE = 7, GROUP_LANGUAGE = 9 };

static int text_set(cape_profile_text* t, const char* s, size_t n){
    t->p = NULL;
    t->len = 0;
    if(!n)
        return CAPE_OK;
    t->p = malloc(n + 1);
    if(!t->p)
        return CAPE_E_CAP;
    memcpy(t->p, s, n);
    t->p[n] = 0;
    t->len = (uint32_t)n;
    return CAPE_OK;
}

static int texts_copy(cape_profile_text** dst, uint32_t* ndst, const cape_profile_text* src, uint32_t n){
    *dst = NULL;
    *ndst = 0;
    if(!n)
        return CAPE_OK;
    *dst = calloc(n, sizeof(**dst));
    if(!*dst)
        return CAPE_E_CAP;
    *ndst = n;
    for(uint32_t i = 0; i < n; i++)
        if(text_set(&(*dst)[i], str(&src[i]), src[i].len) != CAPE_OK)
            return CAPE_E_CAP;
    return CAPE_OK;
}

static void texts_release(cape_profile_text* v, uint32_t n){
    for(uint32_t i = 0; v && i < n; i++)
        free(v[i].p);
    free(v);
}

static void action_release(cape_profile_action* a){
    free(a->name.p);
    free(a->sound_path.p);
    free(a->key_name.p);
    texts_release(a->stroke, a->nstroke);
    free(a->program_path.p);
    for(uint32_t i = 0; a->events && i < a->nevents; i++)
        texts_release(a->events[i].keys, a->events[i].nkeys);
    free(a->events);
    free(a->text.p);
    memset(a, 0, sizeof(*a));
}

// A deep copy (dst is overwritten; on failure it holds what was copied, to release)
static int action_copy(cape_profile_action* dst, const cape_profile_action* src){
    *dst = *src;
    dst->name.p = dst->sound_path.p = dst->key_name.p = dst->program_path.p = dst->text.p = NULL;
    dst->stroke = NULL;
    dst->events = NULL;
    dst->nstroke = dst->nevents = 0;
    if(text_set(&dst->name, str(&src->name), src->name.len) || text_set(&dst->sound_path, str(&src->sound_path), src->sound_path.len)
       || text_set(&dst->key_name, str(&src->key_name), src->key_name.len)
       || text_set(&dst->program_path, str(&src->program_path), src->program_path.len)
       || text_set(&dst->text, str(&src->text), src->text.len) || texts_copy(&dst->stroke, &dst->nstroke, src->stroke, src->nstroke))
        return CAPE_E_CAP;
    if(src->nevents){
        dst->events = calloc(src->nevents, sizeof(*dst->events));
        if(!dst->events)
            return CAPE_E_CAP;
        dst->nevents = src->nevents;
        for(uint32_t i = 0; i < src->nevents; i++){
            dst->events[i] = src->events[i];
            dst->events[i].keys = NULL;
            dst->events[i].nkeys = 0;
            if(texts_copy(&dst->events[i].keys, &dst->events[i].nkeys, src->events[i].keys, src->events[i].nkeys))
                return CAPE_E_CAP;
        }
    }
    return CAPE_OK;
}

static int layer_copy(cape_profile_layer* dst, const cape_profile_layer* src){
    *dst = *src;
    dst->name.p = dst->effect.name.p = NULL;
    dst->keys = NULL;
    dst->nkeys = 0;
    dst->effect.transitions = NULL;
    dst->effect.ntransitions = 0;
    if(text_set(&dst->name, str(&src->name), src->name.len) || text_set(&dst->effect.name, str(&src->effect.name), src->effect.name.len)
       || texts_copy(&dst->keys, &dst->nkeys, src->keys, src->nkeys))
        return CAPE_E_CAP;
    if(src->effect.ntransitions){
        dst->effect.transitions = malloc(src->effect.ntransitions * sizeof(*dst->effect.transitions));
        if(!dst->effect.transitions)
            return CAPE_E_CAP;
        memcpy(dst->effect.transitions, src->effect.transitions, src->effect.ntransitions * sizeof(*dst->effect.transitions));
        dst->effect.ntransitions = src->effect.ntransitions;
    }
    return CAPE_OK;
}

static int new_id(cape_compile_import_opts* o, cape_profile_uuid* u){
    if(!o->random)
        return CAPE_E_ARG;
    o->random(o->random_ctx, u->b);
    u->b[6] = (uint8_t)((u->b[6] & 0x0f) | 0x40);   // version 4
    u->b[8] = (uint8_t)((u->b[8] & 0x3f) | 0x80);   // variant 1
    return CAPE_OK;
}

static int name_taken(const cape_profile_text* names, size_t stride, uint32_t n, const char* name){
    const size_t len = strlen(name);
    for(uint32_t i = 0; i < n; i++){
        const cape_profile_text* t = (const cape_profile_text*)(const void*)((const char*)names + i * stride);
        if(t->len == len && !memcmp(str(t), name, len))
            return 1;
    }
    return 0;
}

// "<label> N", the first N that no name of two lists (n entries, stride bytes apart; the second may be empty) has
static int new_name(cape_profile_text* t, const char* label, const cape_profile_text* names, size_t stride, uint32_t n,
                    const cape_profile_text* more, size_t more_stride, uint32_t nmore){
    char name[64];
    for(unsigned k = 1;; k++){
        snprintf(name, sizeof(name), "%s %u", label, k);
        if(!name_taken(names, stride, n, name) && !name_taken(more, more_stride, nmore, name))
            return text_set(t, name, strlen(name));
    }
}

static const char* action_label(const cape_profile_action* a){
    switch(a->kind){
    case CAPE_PROFILE_MACRO:
        return "Macro";
    case CAPE_PROFILE_TEXT:
        return "Text";
    case CAPE_PROFILE_MEDIA:
        return "Media";
    default:
        break;
    }
    switch(a->remap_group){
    case GROUP_MOUSE:
        return "Mouse button";
    case GROUP_KEYSTROKE:
        return "Keystroke";
    case GROUP_LANGUAGE:
        return "Language keys";
    default:
        return "Keyboard";
    }
}

// The hint of a macro file's start, run and repeat; on_press is the hint of the plain case (start on press, run once). -1 if iCUE
// has none for them.
static int hint_of(const cape_binding_action* a, uint8_t on_press){
    if(a->repeat != 1)
        return -1;
    if(a->start == 0x11)
        return a->run == 1 ? CAPE_PROFILE_HINT_ON_RELEASE : -1;
    if(a->start != 0)
        return -1;
    switch(a->run){
    case 1:
        return on_press;
    case 3:
        return CAPE_PROFILE_HINT_WHILE_PRESSED;
    case 4:
        return CAPE_PROFILE_HINT_TOGGLE;
    default:
        return -1;
    }
}

static void action_defaults(cape_profile_action* a, cape_profile_action_kind kind){
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->repeat_count = 1;
    a->exec_hint = CAPE_PROFILE_HINT_ON_PRESS;
    if(kind == CAPE_PROFILE_REMAP){
        a->exec_hint = CAPE_PROFILE_HINT_ON_BOTH;
        a->sniper_mode = CAPE_PROFILE_SNIPER_WHILE_PRESSED;
        a->holding_interval = 100;
    } else if(kind == CAPE_PROFILE_MACRO){
        a->termination = CAPE_PROFILE_TERMINATE_INTERRUPT;
        a->restart_on_second = 1;
    }
}

static int key_name_set(cape_profile_text* t, uint8_t key){
    const char* name = cape_profile_key_name(key);
    return name ? text_set(t, name, strlen(name)) : CAPE_COMPILE_E_UNCOMPILABLE;
}

// The events of a macro file as the events of iCUE's Macro: the delay of 0 between two key events is its writer's
static int macro_from_events(const cape_binding_action* a, cape_profile_action* out){
    unsigned rows = 0;
    for(unsigned i = 0; i < a->nevents; i++){
        const cape_event e = a->events[i];
        const int kind = cape_ev_kind(e);
        if(kind == CAPE_EV_DELAY && cape_ev_delay_ms(e) == 0 && i > 0 && i + 1 < a->nevents && is_key_event(a->events[i - 1])
           && is_key_event(a->events[i + 1]))
            continue;
        if(kind != CAPE_EV_PRESS && kind != CAPE_EV_RELEASE && kind != CAPE_EV_DELAY && kind != CAPE_EV_RANDOM)
            return CAPE_COMPILE_E_UNCOMPILABLE;
        rows++;
    }
    if(rows > CAPE_BIND_MACRO_ROWS_MAX)
        return CAPE_COMPILE_E_UNCOMPILABLE;
    out->events = calloc(rows ? rows : 1, sizeof(*out->events));
    if(!out->events)
        return CAPE_E_CAP;
    for(unsigned i = 0; i < a->nevents; i++){
        const cape_event e = a->events[i];
        const int kind = cape_ev_kind(e);
        if(kind == CAPE_EV_DELAY && cape_ev_delay_ms(e) == 0 && i > 0 && i + 1 < a->nevents && is_key_event(a->events[i - 1])
           && is_key_event(a->events[i + 1]))
            continue;
        cape_profile_event* ev = &out->events[out->nevents++];
        if(kind == CAPE_EV_DELAY){
            ev->kind = CAPE_PROFILE_DELAY_EVENT;
            ev->delay = cape_ev_delay_ms(e);
        } else if(kind == CAPE_EV_RANDOM){
            ev->kind = CAPE_PROFILE_DELAY_EVENT;
            ev->random = 1;
            ev->delay_min = cape_ev_random_min(e);
            ev->delay_max = cape_ev_random_max(e);
        } else {
            ev->kind = CAPE_PROFILE_KEY_EVENT;
            ev->sub = kind == CAPE_EV_PRESS ? CAPE_PROFILE_SUB_PRESS : CAPE_PROFILE_SUB_RELEASE;
            ev->keys = calloc(1, sizeof(*ev->keys));
            if(!ev->keys)
                return CAPE_E_CAP;
            ev->nkeys = 1;
            const int rc = key_name_set(&ev->keys[0], cape_ev_key(e));
            if(rc != CAPE_OK)
                return rc;
        }
    }
    return CAPE_OK;
}

// The characters of iCUE's Text for a key of the US layout, with or without Shift ("" for none)
static const char* text_char(uint8_t key, int shift, char buf[2]){
    static const char plain[] = "`-=[]\\;',./ \t";
    static const char shifted[] = "~_+{}|:\"<>?";
    static const char* const names[] = {
        "GraveAccentAndTilde", "MinusAndUnderscore", "EqualsAndPlus", "BracketLeft", "BracketRight", "Backslash", "SemicolonAndColon",
        "ApostropheAndDoubleQuote", "CommaAndLessThan", "PeriodAndBiggerThan", "SlashAndQuestionMark", "Space", "Tab",
    };
    static const char digit_shifted[] = ")!@#$%^&*(";
    const char* name = cape_profile_key_name(key);
    if(!name)
        return "";
    const size_t len = strlen(name);
    buf[1] = 0;
    if(len == 1 && name[0] >= 'A' && name[0] <= 'Z'){
        buf[0] = (char)(shift ? name[0] : name[0] - 'A' + 'a');
        return buf;
    }
    if(len == 1 && name[0] >= '0' && name[0] <= '9'){
        buf[0] = shift ? digit_shifted[name[0] - '0'] : name[0];
        return buf;
    }
    if(!strcmp(name, "Enter"))
        return shift ? "" : "\r\n";
    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if(!strcmp(name, names[i])){
            if(shift && i >= sizeof(shifted) - 1)
                return "";
            buf[0] = shift ? shifted[i] : plain[i];
            return buf;
        }
    return "";
}

// The string of a Text from the events of its file; 0 if they are not a Text of iCUE
static int text_from_events(const cape_binding_action* a, cape_profile_action* out){
    const uint8_t lshift = (uint8_t)cape_profile_key("LeftShift", 9);
    // At most two bytes of string per character, and a character takes three events at least
    char* s = malloc(a->nevents + 1);
    if(!s)
        return CAPE_E_CAP;
    size_t n = 0;
    unsigned i = 0, chars = 0;
    int ok = 1;
    uint32_t delay = 0;
    while(ok && i < a->nevents){
        if(chars){
            const int kind = cape_ev_kind(a->events[i]);
            if(kind != CAPE_EV_DELAY && kind != CAPE_EV_LONG){
                ok = 0;
                break;
            }
            const uint32_t d = cape_ev_delay_ms(a->events[i]);
            if(chars > 1 && d != delay)
                ok = 0;
            delay = d;
            i++;
        }
        const cape_event* e = a->events + i;
        const unsigned left = a->nevents - i;
        int shift = left >= 7 && cape_ev_kind(e[0]) == CAPE_EV_PRESS && cape_ev_key(e[0]) == lshift;
        const cape_event* k = shift ? e + 2 : e;
        const unsigned need = shift ? 7 : 3;
        if(left < need || cape_ev_kind(k[0]) != CAPE_EV_PRESS || cape_ev_kind(k[2]) != CAPE_EV_RELEASE
           || cape_ev_key(k[0]) != cape_ev_key(k[2])){
            ok = 0;
            break;
        }
        if(shift && (cape_ev_kind(e[6]) != CAPE_EV_RELEASE || cape_ev_key(e[6]) != lshift)){
            ok = 0;
            break;
        }
        char buf[2];
        const char* c = text_char(cape_ev_key(k[0]), shift, buf);
        if(!*c){
            ok = 0;
            break;
        }
        const size_t cl = strlen(c);
        memcpy(s + n, c, cl);
        n += cl;
        chars++;
        i += need;
    }
    int rc = ok ? text_set(&out->text, s, n) : CAPE_COMPILE_E_UNCOMPILABLE;
    free(s);
    out->char_delay = chars > 1 ? delay : 0;
    return rc;
}

// Whether two compiled actions are the same, but for the subtype when same_events
static int compiled_equal(const cape_binding_action* x, const cape_binding_action* y, int any_subtype){
    cape_binding_action a = *x;
    if(any_subtype)
        a.subtype = y->subtype;
    return cape_binding_action_equal(&a, y);
}

// Whether action compiles to exactly what the files hold for its key (any_subtype: but for the subtype of its macro file): 1 or 0,
// CAPE_E_CAP if memory ran out
static int compiles_to(const cape_profile_action* action, const cape_binding_action* files, int any_subtype){
    cape_binding_action c;
    const int rc = cape_compile_action(action, &c, NULL);
    if(rc == CAPE_E_CAP)
        return rc;
    if(rc != CAPE_OK)
        return 0;
    const int same = compiled_equal(&c, files, any_subtype);
    free(c.events);
    return same;
}

// The result of an inversion that compiles back to the files (same) or not
static int exact(int same){
    return same < 0 ? same : same ? CAPE_OK : CAPE_COMPILE_E_UNCOMPILABLE;
}

// The action iCUE makes for what the files hold for a key (not NATIVE). Returns CAPE_OK, CAPE_COMPILE_E_UNCOMPILABLE (not one of
// iCUE's) or CAPE_E_CAP; out is to be released in any case.
static int invert(const cape_binding_action* a, int convert, int* converted, cape_profile_action* out){
    *converted = 0;
    if(a->kind == CAPE_BIND_REMAP){
        const char* media = cape_profile_media_name(a->dst);
        if(media){
            action_defaults(out, CAPE_PROFILE_MEDIA);
            for(uint8_t m = 0; m < MEDIA_COUNT; m++)
                if(!strcmp(media, media_names[m]))
                    out->media = m;
        } else {
            action_defaults(out, CAPE_PROFILE_REMAP);
            const unsigned sets = cape_binding_keysets(a->dst);
            if(a->dst >= CAPE_BIND_MOUSE_FIRST && a->dst <= CAPE_BIND_MOUSE_LAST)
                out->remap_group = GROUP_MOUSE;
            else if((sets & CAPE_BIND_KEYSET_LANGUAGE) && !(sets & CAPE_BIND_KEYSET_KEYBOARD))
                out->remap_group = GROUP_LANGUAGE;
            const int rc = key_name_set(&out->key_name, a->dst);
            if(rc != CAPE_OK)
                return rc;
        }
        return exact(compiles_to(out, a, 0));
    }
    int rc = CAPE_COMPILE_E_UNCOMPILABLE;
    if(a->subtype == CAPE_MACRO_SHORTCUT){
        action_defaults(out, CAPE_PROFILE_REMAP);
        const int imitate = a->run == 0x84
                            || (a->nevents == 3 && !(cape_ev_kind(a->events[1]) == CAPE_EV_DELAY && cape_ev_delay_ms(a->events[1]) == 0));
        if(imitate && a->nevents == 3 && cape_ev_kind(a->events[0]) == CAPE_EV_PRESS){
            out->holding_enabled = 1;
            out->exec_hint = CAPE_PROFILE_HINT_ON_PRESS;
            if(a->run == 0x84)
                out->holding_type = CAPE_PROFILE_HOLD_TOGGLE;
            else {
                const int hint = hint_of(a, CAPE_PROFILE_HINT_ON_PRESS);
                out->exec_hint = (uint8_t)(hint < 0 ? CAPE_PROFILE_HINT_ON_PRESS : hint);
                out->holding_interval = cape_ev_delay_ms(a->events[1]);
            }
            rc = key_name_set(&out->key_name, cape_ev_key(a->events[0]));
        } else if(!imitate && a->nevents % 4 == 3){
            // n presses and n releases, a delay of 0 between each two
            const unsigned n = (a->nevents + 1) / 4;
            const int hint = hint_of(a, CAPE_PROFILE_HINT_ON_BOTH);
            out->remap_group = GROUP_KEYSTROKE;
            out->exec_hint = (uint8_t)(hint < 0 ? CAPE_PROFILE_HINT_ON_BOTH : hint);
            out->stroke = calloc(n, sizeof(*out->stroke));
            rc = out->stroke ? CAPE_OK : CAPE_E_CAP;
            for(unsigned i = 0; rc == CAPE_OK && i < n; i++){
                out->nstroke++;
                rc = key_name_set(&out->stroke[i], cape_ev_key(a->events[2 * i]));
            }
        }
    } else if(a->subtype == CAPE_MACRO_TEXT){
        action_defaults(out, CAPE_PROFILE_TEXT);
        const int hint = hint_of(a, CAPE_PROFILE_HINT_ON_PRESS);
        out->exec_hint = (uint8_t)(hint < 0 ? CAPE_PROFILE_HINT_ON_PRESS : hint);
        rc = text_from_events(a, out);
    } else if(a->subtype == CAPE_MACRO_MACRO){
        action_defaults(out, CAPE_PROFILE_MACRO);
        const int hint = hint_of(a, CAPE_PROFILE_HINT_ON_PRESS);
        out->exec_hint = (uint8_t)(hint < 0 ? CAPE_PROFILE_HINT_ON_PRESS : hint);
        rc = macro_from_events(a, out);
    }
    if(rc == CAPE_OK)
        rc = exact(compiles_to(out, a, 0));
    if(rc != CAPE_COMPILE_E_UNCOMPILABLE)
        return rc;
    if(!convert || a->subtype == CAPE_MACRO_MACRO)
        return CAPE_COMPILE_E_UNCOMPILABLE;
    // Converting: a Macro of the same events
    action_release(out);
    action_defaults(out, CAPE_PROFILE_MACRO);
    const int hint = hint_of(a, CAPE_PROFILE_HINT_ON_PRESS);
    out->exec_hint = (uint8_t)(hint < 0 ? CAPE_PROFILE_HINT_ON_PRESS : hint);
    rc = macro_from_events(a, out);
    if(rc == CAPE_OK)
        rc = exact(compiles_to(out, a, 1));
    *converted = rc == CAPE_OK;
    return rc;
}

// Rule 2 with an action of the base of the same kind on the key: its name and identity, and its values where they give the same
static int keep_from_base(cape_profile_action* a, const cape_binding_action* files, const cape_profile_action* b){
    free(a->name.p);
    if(text_set(&a->name, str(&b->name), b->name.len) != CAPE_OK)
        return CAPE_E_CAP;
    a->id = b->id;
    a->restart_on_second = b->restart_on_second;
    a->sniper_mode = b->sniper_mode;
    const uint8_t hint = a->exec_hint;
    // iCUE reloads a plain Keystroke as OnBoth. An older Keyboard action's OnPress compiles to the same files,
    // but carrying it over would leave metadata that iCUE changes when loading the edited action.
    if(a->kind != CAPE_PROFILE_REMAP || !a->nstroke || hint != CAPE_PROFILE_HINT_ON_BOTH)
        a->exec_hint = b->exec_hint;
    int same = compiles_to(a, files, 0);
    if(same < 0)
        return same;
    if(!same)
        a->exec_hint = hint;
    if(a->kind == CAPE_PROFILE_REMAP && a->key_name.len == b->key_name.len && !memcmp(str(&a->key_name), str(&b->key_name), b->key_name.len))
        a->remap_group = b->remap_group;
    if(a->kind == CAPE_PROFILE_REMAP && a->holding_enabled){
        const uint32_t ms = a->holding_interval;
        a->holding_interval = b->holding_interval;
        same = compiles_to(a, files, 0);
        if(same < 0)
            return same;
        if(!same)
            a->holding_interval = ms;
    }
    if(a->kind == CAPE_PROFILE_TEXT){
        cape_profile_text t = a->text;
        if(text_set(&a->text, str(&b->text), b->text.len) != CAPE_OK){
            a->text = t;
            return CAPE_E_CAP;
        }
        same = compiles_to(a, files, 0);
        if(same > 0)
            free(t.p);
        else {
            free(a->text.p);
            a->text = t;
        }
        if(same < 0)
            return same;
    }
    return CAPE_OK;
}

static int import_in_order(const cape_binding_model* bm, const cape_profile_profile* base, cape_compile_import_opts* o,
                           cape_profile_profile* out, int* order, char* why){
    const uint32_t nbase = base ? base->nbindings : 0;
    out->bindings = calloc((size_t)nbase + CAPE_BIND_K95_KEYS, sizeof(*out->bindings));
    if(!out->bindings)
        return CAPE_E_CAP;
    uint8_t done[CAPE_BIND_K95_KEYS] = { 0 };
    // which action of the base each key keeps or updates, and the actions of the base on no key, in the order of the base; then
    // the keys the base has no action on
    unsigned norder = 0;
    for(uint32_t i = 0; i < nbase; i++){
        const int k = key_of(&base->bindings[i].key);
        if(!base->bindings[i].key.len)
            order[norder++] = (int)i;
        else if(k >= 0 && k < CAPE_BIND_K95_KEYS && !done[k]){
            done[k] = 1;
            order[norder++] = (int)i;
        }
    }
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        if(!done[k] && bm->key[k].kind != CAPE_BIND_NATIVE)
            order[norder++] = -1 - (int)k;
    for(unsigned j = 0; j < norder; j++){
        const cape_profile_binding* b = order[j] >= 0 ? &base->bindings[order[j]] : NULL;
        cape_profile_binding* nb = &out->bindings[out->nbindings];
        // 0. an action of the base on no key: the files say nothing of it, it is kept as it is
        if(b && !b->key.len){
            out->nbindings++;
            nb->layer = b->layer;
            nb->event = b->event;
            nb->distance = b->distance;
            if(action_copy(&nb->action, &b->action) != CAPE_OK)
                return CAPE_E_CAP;
            o->kept++;
            continue;
        }
        const unsigned key = b ? (unsigned)key_of(&b->key) : (unsigned)(-1 - order[j]);
        const cape_binding_action* files = &bm->key[key];
        // 1. the base's action, whole
        const int same = b ? compiles_to(&b->action, files, 0) : 0;
        if(same < 0)
            return same;
        if(same){
            out->nbindings++;
            nb->layer = b->layer;
            nb->event = b->event;
            nb->distance = b->distance;
            if(text_set(&nb->key, str(&b->key), b->key.len) != CAPE_OK || action_copy(&nb->action, &b->action) != CAPE_OK)
                return CAPE_E_CAP;
            o->kept++;
            continue;
        }
        if(files->kind == CAPE_BIND_NATIVE)
            continue;
        // 2. the files inverted
        out->nbindings++;
        int converted;
        int rc = invert(files, o->convert, &converted, &nb->action);
        if(rc == CAPE_COMPILE_E_UNCOMPILABLE)
            return fail(why, rc, "the %s of the key %s is no action of iCUE", files->kind == CAPE_BIND_REMAP ? "remap" : "macro file",
                        cape_profile_key_name((uint8_t)key) ? cape_profile_key_name((uint8_t)key) : "?");
        if(rc != CAPE_OK)
            return rc;
        o->converted += (unsigned)converted;
        const char* source = cape_profile_key_name((uint8_t)key);
        if(!source)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an action on the key %u, which iCUE has no name for", key);
        if(b)
            rc = text_set(&nb->key, str(&b->key), b->key.len);
        else
            rc = text_set(&nb->key, source, strlen(source));
        if(rc != CAPE_OK)
            return rc;
        if(b && b->action.kind == nb->action.kind)
            rc = keep_from_base(&nb->action, files, &b->action);
        else {
            rc = new_id(o, &nb->action.id);
            if(rc == CAPE_OK)
                // not the name of an action of the base either: one kept whole may come later in the order
                rc = new_name(&nb->action.name, action_label(&nb->action), &out->bindings[0].action.name, sizeof(*out->bindings),
                              out->nbindings - 1, nbase ? &base->bindings[0].action.name : NULL, sizeof(*base->bindings), nbase);
        }
        if(rc != CAPE_OK)
            return rc;
    }
    // 'Imitate holding key' with an interval the file cannot say
    for(size_t i = 0; i < o->nhold; i++){
        cape_profile_action* a = NULL;
        for(uint32_t j = 0; j < out->nbindings; j++)
            if(key_of(&out->bindings[j].key) == o->hold[i].key)
                a = &out->bindings[j].action;
        if(!a || a->kind != CAPE_PROFILE_REMAP || !a->holding_enabled || a->holding_type != CAPE_PROFILE_HOLD_ON_PRESS)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an interval for the key %u, which has no 'Imitate holding key' on press",
                        o->hold[i].key);
        const uint32_t ms = a->holding_interval;
        a->holding_interval = o->hold[i].ms;
        const int same = compiles_to(a, &bm->key[o->hold[i].key], 0);
        if(same < 0)
            return same;
        if(!same){
            a->holding_interval = ms;
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "an interval of %u ms for the key %u, not what its file holds",
                        (unsigned)o->hold[i].ms, o->hold[i].key);
        }
    }
    return CAPE_OK;
}

static int import_bindings(const cape_binding_model* bm, const cape_profile_profile* base, cape_compile_import_opts* o,
                           cape_profile_profile* out, char* why){
    const size_t nbase = base ? base->nbindings : 0;
    int* order = malloc((nbase + CAPE_BIND_K95_KEYS) * sizeof(*order));
    if(!order)
        return CAPE_E_CAP;
    const int rc = import_in_order(bm, base, o, out, order, why);
    free(order);
    return rc;
}

static void colour_of(cape_profile_colour* c, const uint8_t rgb[3]){
    memset(c, 0, sizeof(*c));
    for(int i = 0; i < 3; i++)
        c->c[i] = rgb[i] / 255.0;
    c->c[3] = 1;
}

static void import_indicators(const uint8_t ind[CAPE_COMPILE_IND_SIZE], const cape_profile_profile* base, cape_profile_profile* out){
    static const uint8_t defaults[CAPE_COMPILE_IND_SIZE] = { 0xff, 0, 0, 0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0, 0 };
    uint8_t was[CAPE_COMPILE_IND_SIZE];
    if(base){
        out->profile = base->profile;
        out->brightness = base->brightness;
        out->winlock_on = base->winlock_on;
        out->winlock_off = base->winlock_off;
        cape_compile_indicators(base, was);
    } else {
        memcpy(was, defaults, sizeof(was));
        colour_of(&out->brightness, defaults + 3);
    }
    if(memcmp(was + 3, ind + 3, 3))
        colour_of(&out->brightness, ind + 3);
    if(!memcmp(ind, was, 3) && !memcmp(ind + 6, was + 6, 6))
        return;
    // the three optional ones, set together as iCUE does
    out->profile.present = out->winlock_on.present = out->winlock_off.present = 1;
    colour_of(&out->profile.colour, ind);
    colour_of(&out->winlock_on.colour, ind + 6);
    colour_of(&out->winlock_off.colour, ind + 9);
}

static const cape_slot_file* lighting_file(const cape_slot_files* files, const char* name){
    return cape_slot_files_find(files, name);
}

// Whether all lighting files of the slot match the compiled model byte for byte.
static int lighting_same(const cape_slot_files* files, const cape_compile_lighting* lt){
    size_t made = 0;
    for(size_t i = 0; i < lt->nfiles; i++){
        const cape_slot_file* f = lighting_file(files, lt->file[i].name);
        if(!f || f->len != lt->file[i].len || memcmp(f->data, lt->file[i].data, f->len))
            return 0;
        made += f->len;
    }
    unsigned count = 0;
    size_t bytes = 0;
    for(size_t i = 0; i < files->n; i++)
        if(!strncmp(files->file[i].name, "lght_", 5) || !strcmp(files->file[i].name, CAPE_FILE_LAYERCOUNT)){
            count++;
            bytes += files->file[i].len;
        }
    return count == lt->count && bytes == lt->bytes && made == bytes;
}

static int import_lighting(const cape_slot_files* files, const cape_profile_profile* base, cape_compile_import_opts* o,
                           cape_profile_profile* out, char* why){
    static cape_compile_lighting lt;
    if(o->lighting){
        cape_lightmodel copy = {0};
        if(!o->lighting->known) return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "unknown replacement lighting");
        const int rc = cape_lightmodel_copy(o->lighting, &copy);
        if(rc != CAPE_OK) return rc;
        out->advanced = base ? base->advanced : 1;
        out->layers = copy.layers;
        out->nlayers = copy.count;
        return CAPE_OK;
    }
    if(base && cape_compile_lighting_build(base, o->layout, &lt, NULL) == CAPE_OK && lighting_same(files, &lt)){
        out->advanced = base->advanced;
        if(base->nlayers){
            out->layers = calloc(base->nlayers, sizeof(*out->layers));
            if(!out->layers)
                return CAPE_E_CAP;
            for(uint32_t i = 0; i < base->nlayers; i++){
                out->nlayers++;
                if(layer_copy(&out->layers[i], &base->layers[i]) != CAPE_OK)
                    return CAPE_E_CAP;
            }
        }
        o->effects = lt.effects;
        return CAPE_OK;
    }
    const cape_slot_file* cnt = lighting_file(files, CAPE_FILE_LAYERCOUNT);
    uint32_t n;
    if(!cnt || cape_layercount_parse(cnt->data, cnt->len, &n) != CAPE_OK || n > CAPE_LAYERS_MAX)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no lighting iCUE writes (lghtcnt.cnt)");
    uint8_t all[CAPE_PROFILE_LAYER_MAX];
    const unsigned nall = cape_profile_layout_all(o->layout, all, NULL);
    if(n && !nall)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "lighting layers on an unknown layout (%d)", o->layout);
    if(n){
        // zeroed, so that the layers not made yet can be freed
        out->layers = calloc(n, sizeof(*out->layers));
        if(!out->layers)
            return CAPE_E_CAP;
        out->nlayers = n;
    }
    for(uint32_t f = 0; f < n; f++){
        char name[10];
        cape_light_filename(f, 'd', name);
        const cape_slot_file* d = lighting_file(files, name);
        cape_light_filename(f, 'k', name);
        const cape_slot_file* k = lighting_file(files, name);
        cape_light_filename(f, 'r', name);
        const cape_slot_file* r = lighting_file(files, name);
        const cape_layer_kind kind = d ? cape_light_layer_kind(d->data, d->len, NULL, 0) : CAPE_LAYER_UNKNOWN;
        if(kind == CAPE_LAYER_PREDEFINED || kind == CAPE_LAYER_CUSTOM)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "layers of effects, which are not made from the files");
        uint8_t cells[CAPE_CELLS];
        cape_rgba rgba[CAPE_COLORS_COUNT];
        const int ncells = k ? cape_keylist_parse(k->data, k->len, cells, sizeof(cells)) : -1;
        if(kind != CAPE_LAYER_STATIC || ncells < 0 || !r || cape_colors_parse(r->data, r->len, rgba) != CAPE_OK)
            return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a lighting layer %u iCUE does not write", (unsigned)f);
        // the list of iCUE goes from the top: the last file first
        cape_profile_layer* l = &out->layers[n - 1 - f];
        const int every = ncells == (int)nall && !memcmp(&rgba[nall], &rgba[0], sizeof(rgba[0]));
        l->option = every ? CAPE_PROFILE_OPTION_ALL : CAPE_PROFILE_OPTION_CONCRETE;
        l->enabled = 1;
        l->start_with_profile = 1;
        l->stop_after_times = -1;
        if(!every && ncells){
            l->keys = calloc((size_t)ncells, sizeof(*l->keys));
            if(!l->keys)
                return CAPE_E_CAP;
            for(int i = 0; i < ncells; i++){
                const char* light = cape_profile_light_name(cells[i], o->layout);
                if(!light)
                    return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "a cell %u in layer %u that iCUE does not list", cells[i], (unsigned)f);
                l->nkeys++;
                if(text_set(&l->keys[i], light, strlen(light)) != CAPE_OK)
                    return CAPE_E_CAP;
            }
        }
        cape_profile_effect* e = &l->effect;
        e->kind = CAPE_PROFILE_STATIC;
        e->duration = 1000;
        e->brightness = 10;
        e->opacity = rgba[0].a;
        e->transitions = calloc(1, sizeof(*e->transitions));
        if(!e->transitions)
            return CAPE_E_CAP;
        e->ntransitions = 1;
        const uint8_t rgb[3] = { rgba[0].r, rgba[0].g, rgba[0].b };
        colour_of(&e->transitions[0].colour, rgb);
        int rc = new_id(o, &l->id);
        if(rc == CAPE_OK)
            rc = new_id(o, &e->id);
        if(rc != CAPE_OK)
            return rc;
    }
    // names, from the top
    for(uint32_t i = 0; i < out->nlayers; i++)
        if(new_name(&out->layers[i].name, "Static Color", &out->layers[0].name, sizeof(*out->layers), i, NULL, 0, 0) != CAPE_OK)
            return CAPE_E_CAP;
    out->advanced = base ? base->advanced : 1;
    if(cape_compile_lighting_build(out, o->layout, &lt, NULL) != CAPE_OK || !lighting_same(files, &lt))
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "lighting layers that are not the ones iCUE writes for their keys and colours");
    return CAPE_OK;
}

// Whether the files of the bindings of a model are those of the slot, byte for byte: 1 or 0 (first names the first that is not),
// CAPE_E_CAP if memory ran out
static int bindings_laid_out(const cape_slot_files* files, const cape_profile_profile* p, char first[CAPE_SLOT_NAME]){
    cape_binding_model m;
    cape_slot_files made;
    memset(&made, 0, sizeof(made));
    int rc = cape_compile_bindings(p, &m, NULL);
    if(rc == CAPE_OK)
        rc = cape_binding_build(&m, &made);
    cape_binding_free(&m);
    int same = rc == CAPE_OK;
    for(size_t i = 0; same && i < made.n; i++){
        const cape_slot_file* x = cape_slot_files_find(files, made.file[i].name);
        same = x && x->len == made.file[i].len && !memcmp(x->data, made.file[i].data, x->len);
        if(!same)
            snprintf(first, CAPE_SLOT_NAME, "%s", made.file[i].name);
    }
    cape_slot_files_free(&made);
    return rc == CAPE_E_CAP ? rc : same;
}

int cape_compile_import(const cape_slot_files* files, const cape_profile_profile* base, cape_compile_import_opts* opts,
                        cape_profile_profile* out, char* why){
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!files || !opts)
        return CAPE_E_ARG;
    opts->converted = opts->kept = 0;
    opts->effects = opts->relaid = 0;
    const cape_slot_file* info = cape_slot_files_find(files, CAPE_FILE_INFO);
    if(!info || info->len < CAPE_COMPILE_IND_OFFSET + CAPE_COMPILE_IND_SIZE)
        return fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "no PROFILE.I");
    // what the files do not hold
    if(base){
        out->m1 = base->m1;
        out->m2 = base->m2;
        out->m3 = base->m3;
        out->side_lighting = base->side_lighting;
        out->disable_encryption = base->disable_encryption;
        out->disable_touchpad = base->disable_touchpad;
        out->swap_fkeys = base->swap_fkeys;
        out->hw_actions_types = base->hw_actions_types;
        out->active_preset = base->active_preset;
        out->preset_active = base->preset_active;
    } else {
        out->m1 = new_m1;
        out->m2 = new_m2;
        out->m3 = new_m3;
        out->side_lighting = 1;
        out->swap_fkeys = 1;
    }
    cape_binding_model bm;
    int rc = cape_binding_import_files(files, &bm);
    if(rc == CAPE_OK && bm.state != CAPE_BIND_OK)
        rc = fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "bindings iCUE does not write: %s", bm.reason);
    else if(rc == CAPE_OK && (bm.winlock & ~CAPE_WINLOCK_KNOWN))
        rc = fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "Win Lock options %02x, of which iCUE has the low four", bm.winlock);
    if(rc == CAPE_OK){
        out->disable_win = (bm.winlock & CAPE_WINLOCK_WINKEY) != 0;
        out->disable_alt_tab = (bm.winlock & CAPE_WINLOCK_ALT_TAB) != 0;
        out->disable_alt_f4 = (bm.winlock & CAPE_WINLOCK_ALT_F4) != 0;
        out->disable_shift_tab = (bm.winlock & CAPE_WINLOCK_SHIFT_TAB) != 0;
        rc = import_bindings(&bm, base, opts, out, why);
    }
    cape_binding_free(&bm);
    if(rc == CAPE_OK && !opts->converted){
        // the files as iCUE lays them out: the order of the MAP and the DAT, the numbering of the macro files
        char first[CAPE_SLOT_NAME] = "";
        const int same = bindings_laid_out(files, out, first);
        if(same < 0)
            rc = same;
        else if(!same && opts->convert)
            opts->relaid = 1;
        else if(!same)
            rc = fail(why, CAPE_COMPILE_E_UNCOMPILABLE, "%s is not laid out as iCUE writes it", first);
    }
    if(rc == CAPE_OK){
        import_indicators(info->data + CAPE_COMPILE_IND_OFFSET, base, out);
        rc = import_lighting(files, base, opts, out, why);
    }
    if(rc != CAPE_OK)
        cape_profile_free(out);
    return rc;
}
