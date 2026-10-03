#include "cape_binding.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(char* out, size_t cap, const char* fmt, ...){
    if(!out || !cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

static void clear_action(cape_binding_action* a){
    free(a->events);
    memset(a, 0, sizeof(*a));
}

static void clear_actions(cape_binding_model* m){
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        clear_action(&m->key[k]);
    m->remaps = m->macros = 0;
}

void cape_binding_free(cape_binding_model* m){
    if(!m)
        return;
    clear_actions(m);
    memset(m, 0, sizeof(*m));
}

void cape_binding_init(cape_binding_model* m){
    if(!m)
        return;
    memset(m, 0, sizeof(*m));
    m->state = CAPE_BIND_OK;
    m->winlock = CAPE_WINLOCK_DEFAULT;
}

// What the slot is, when it is not a model: the actions are dropped, the reason stays
static void set_state(cape_binding_model* m, cape_binding_state state, const char* fmt, ...){
    clear_actions(m);
    m->state = state;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->reason, sizeof(m->reason), fmt, ap);
    va_end(ap);
}

// A model holds one action per key, so every model fits the macro files a slot holds (no check needed when editing or building)
_Static_assert(CAPE_SLOT_MACROS_MAX >= CAPE_BIND_K95_KEYS, "a macro file for every key");

static int is_mouse(uint8_t dst){
    return dst >= CAPE_BIND_MOUSE_FIRST && dst <= CAPE_BIND_MOUSE_LAST;
}

static unsigned flags_of(uint8_t subtype, uint8_t start, uint8_t run, uint8_t repeat){
    unsigned f = 0;
    // iCUE's 'Imitate holding key' in its toggle mode: a Keystroke run 84, repeat 0, and nothing else with 84
    if(subtype == CAPE_MACRO_SHORTCUT && start == 0x00 && run == 0x84 && repeat == 0)
        return 0;
    // iCUE's hardware Macro has four alternatives, (00,1) on press, (11,1) on release, (00,3) while pressed, (00,4) toggle
    // and no repeat: on release with another run type and repeats are not offered
    if(repeat != 1 || (start == 0x11 && run != 1))
        f |= CAPE_BIND_F_LOCKED;
    // iCUE writes a Shortcut and a Text only on press and uninterrupted (our own tools wrote the others)
    if(subtype != CAPE_MACRO_MACRO && (start != 0x00 || run != 1))
        f |= CAPE_BIND_F_NON_ICUE;
    return f;
}

static int zero(const uint8_t* p, size_t n){
    for(size_t i = 0; i < n; i++)
        if(p[i])
            return 0;
    return 1;
}

// The first thing wrong with the events for the model, or NULL: a kind nobody has decoded, or a key outside the formats
static const char* events_outside(const cape_event* ev, unsigned n){
    for(unsigned i = 0; i < n; i++){
        const int kind = cape_ev_kind(ev[i]);
        if(kind == CAPE_EV_UNKNOWN)
            return "an event nobody has decoded";
        if((kind == CAPE_EV_PRESS || kind == CAPE_EV_RELEASE) && cape_ev_key(ev[i]) >= CAPE_BIND_K95_KEYS)
            return "an event of a key outside the formats";
    }
    return NULL;
}

// Whether a file parsed as p builds back to the same bytes (what makes the model exact)
// Whether two lists of events are the same, event by event (their own bytes only)
static int same_events(const cape_event* a, const cape_event* b, unsigned n){
    for(unsigned i = 0; i < n; i++)
        if(!cape_ev_equal(a[i], b[i]))
            return 0;
    return 1;
}

static int same_bytes(const uint8_t* built, int nbuilt, const uint8_t* data, size_t len){
    return nbuilt >= 0 && (size_t)nbuilt == len && !memcmp(built, data, len);
}

static int import_files(const cape_slot_files* files, cape_binding_model* out){
    const cape_slot_file* map = cape_slot_files_find(files, CAPE_FILE_MAP);
    const cape_slot_file* dat = cape_slot_files_find(files, CAPE_FILE_DAT);
    if(!map || !dat){
        set_state(out, CAPE_BIND_BROKEN, "%s is missing", !dat ? CAPE_FILE_DAT : CAPE_FILE_MAP);
        return CAPE_OK;
    }
    const int ndat = cape_dat_parse(dat->data, dat->len, NULL, 0);
    if(ndat < 0){
        set_state(out, CAPE_BIND_BROKEN, "PROFILE.DAT is not valid (%s)", cape_strerror(ndat));
        return CAPE_OK;
    }
    cape_dat_winlock(dat->data, dat->len, &out->winlock);
    out->winlock_read = 1;
    const int nmap = cape_map_parse(map->data, map->len, NULL, 0);
    if(nmap < 0){
        set_state(out, CAPE_BIND_BROKEN, "PROFILE.MAP is not valid (%s)", cape_strerror(nmap));
        return CAPE_OK;
    }

    int rc = CAPE_OK;
    // Room to build any of the files back: the largest of them
    size_t room = map->len > dat->len ? map->len : dat->len;
    for(size_t i = 0; i < files->n; i++)
        if(files->file[i].len > room)
            room = files->file[i].len;
    cape_map_entry* remaps = malloc((size_t)(nmap ? nmap : 1) * sizeof(*remaps));
    cape_dat_entry* entries = malloc((size_t)(ndat ? ndat : 1) * sizeof(*entries));
    uint8_t* built = malloc(room);
    const cape_slot_file** mfile = calloc((size_t)(ndat ? ndat : 1), sizeof(*mfile));
    if(!remaps || !entries || !built || !mfile){
        rc = CAPE_E_CAP;
        goto done;
    }
    cape_map_parse(map->data, map->len, remaps, (size_t)nmap);
    cape_dat_parse(dat->data, dat->len, entries, (size_t)ndat);

    // First what makes the files impossible to keep: the macro files the DAT names must all be there (the file system has no
    // listing, so a name is the only way to find one), and there must not be more of them than a slot can hold
    unsigned distinct = 0;
    for(int i = 0; i < ndat; i++){
        char name[5];
        cape_dat_name(&entries[i], name);
        if(cape_macro_filename_parse(name) < 0){   // M and three lower-case hex digits, as iCUE names them (M009, M00a...)
            set_state(out, CAPE_BIND_BROKEN, "PROFILE.DAT entry %d names a file that is not a macro file", i);
            goto done;
        }
        mfile[i] = cape_slot_files_find(files, name);
        if(!mfile[i]){
            set_state(out, CAPE_BIND_BROKEN, "%s, which PROFILE.DAT names, is missing", name);
            goto done;
        }
        int first = 1;
        for(int j = 0; j < i; j++)
            if(mfile[j] == mfile[i])
                first = 0;
        distinct += (unsigned)first;
    }
    if(distinct > CAPE_SLOT_MACROS_MAX){
        set_state(out, CAPE_BIND_BROKEN, "PROFILE.DAT names more than %d macro files", CAPE_SLOT_MACROS_MAX);
        goto done;
    }
    out->files = distinct;

    // Then what the model cannot say: the files can be kept as they are, not edited
    if(!same_bytes(built, cape_map_build(remaps, (size_t)nmap, built, room), map->data, map->len)){
        set_state(out, CAPE_BIND_RAW, "PROFILE.MAP does not build back to the same bytes");
        goto done;
    }
    if(!same_bytes(built, cape_dat_build_ex(out->winlock, entries, (size_t)ndat, built, room), dat->data, dat->len)){
        set_state(out, CAPE_BIND_RAW, "PROFILE.DAT does not build back to the same bytes");
        goto done;
    }
    for(int i = 0; i < nmap; i++){
        const cape_map_entry* e = &remaps[i];
        if(e->flags != CAPE_MAP_FLAG_REMAP){
            set_state(out, CAPE_BIND_RAW, "PROFILE.MAP entry %d has flags %02x, not a remap iCUE writes", i, e->flags);
            goto done;
        }
        if(e->src >= CAPE_BIND_K95_KEYS || (e->dst >= CAPE_BIND_K95_KEYS && !is_mouse(e->dst))){
            set_state(out, CAPE_BIND_RAW, "PROFILE.MAP entry %d (%02x to %02x) is outside the formats", i, e->src, e->dst);
            goto done;
        }
        if(out->key[e->src].kind != CAPE_BIND_NATIVE){
            set_state(out, CAPE_BIND_RAW, "PROFILE.MAP remaps key %02x twice", e->src);
            goto done;
        }
        out->key[e->src].kind = CAPE_BIND_REMAP;
        out->key[e->src].dst = e->dst;
        out->remaps++;
    }
    for(int i = 0; i < ndat; i++){
        const cape_dat_entry* e = &entries[i];
        const cape_slot_file* f = mfile[i];
        if(e->key >= CAPE_BIND_K95_KEYS){
            set_state(out, CAPE_BIND_RAW, "PROFILE.DAT entry %d is for key %02x, outside the formats", i, e->key);
            goto done;
        }
        if(out->key[e->key].kind != CAPE_BIND_NATIVE){
            set_state(out, CAPE_BIND_RAW, "key %02x has two actions (%s)", e->key,
                      out->key[e->key].kind == CAPE_BIND_REMAP ? "in PROFILE.MAP and PROFILE.DAT" : "twice in PROFILE.DAT");
            goto done;
        }
        if(!zero(e->reserved, sizeof(e->reserved))){
            set_state(out, CAPE_BIND_RAW, "PROFILE.DAT entry %d has bytes where iCUE writes zeros", i);
            goto done;
        }
        if(f->len != e->size){
            set_state(out, CAPE_BIND_RAW, "%s is %zu bytes, PROFILE.DAT says %u", f->name, f->len, (unsigned)e->size);
            goto done;
        }
        cape_macro_hdr hdr;
        const int n = cape_macro_parse(f->data, f->len, &hdr, NULL, 0);
        if(n < 0){
            set_state(out, CAPE_BIND_RAW, "%s is not a valid macro file (%s)", f->name, cape_strerror(n));
            goto done;
        }
        if(hdr.subtype != CAPE_MACRO_MACRO && hdr.subtype != CAPE_MACRO_SHORTCUT && hdr.subtype != CAPE_MACRO_TEXT){
            set_state(out, CAPE_BIND_RAW, "%s has subtype %u, which iCUE does not write for this keyboard", f->name, hdr.subtype);
            goto done;
        }
        if(hdr.reserved[0] || hdr.reserved[1] || hdr.reserved[2]){
            set_state(out, CAPE_BIND_RAW, "%s has bytes where iCUE writes zeros", f->name);
            goto done;
        }
        if(e->start != 0x00 && e->start != 0x11){
            set_state(out, CAPE_BIND_RAW, "PROFILE.DAT entry %d has start condition %02x", i, e->start);
            goto done;
        }
        if(e->runtype != 1 && e->runtype != 3 && e->runtype != 4 && e->runtype != 0x84){
            set_state(out, CAPE_BIND_RAW, "PROFILE.DAT entry %d has run type %u", i, e->runtype);
            goto done;
        }
        cape_binding_action* a = &out->key[e->key];
        a->events = malloc((size_t)(n ? n : 1) * sizeof(*a->events));
        if(!a->events){
            rc = CAPE_E_CAP;
            goto done;
        }
        cape_macro_parse(f->data, f->len, &hdr, a->events, (size_t)n);
        const char* bad = events_outside(a->events, (unsigned)n);
        if(bad){
            set_state(out, CAPE_BIND_RAW, "%s has %s", f->name, bad);
            goto done;
        }
        if(!same_bytes(built, cape_macro_build(&hdr, a->events, (size_t)n, built, room), f->data, f->len)){
            set_state(out, CAPE_BIND_RAW, "%s does not build back to the same bytes", f->name);
            goto done;
        }
        a->kind = CAPE_BIND_MACRO;
        a->subtype = hdr.subtype;
        a->start = e->start;
        a->run = e->runtype;
        a->repeat = e->repeat;
        a->nevents = (unsigned)n;
        memcpy(a->file, f->name, 5);
        a->flags = flags_of(a->subtype, a->start, a->run, a->repeat);
        for(int j = 0; j < i; j++)
            if(mfile[j] == f){
                a->flags |= CAPE_BIND_F_SHARED;
                out->key[entries[j].key].flags |= CAPE_BIND_F_SHARED;
            }
        out->macros++;
    }

done:
    free(remaps);
    free(entries);
    free(built);
    free(mfile);
    return rc;
}

int cape_binding_import_files(const cape_slot_files* files, cape_binding_model* out){
    if(!files || !out)
        return CAPE_E_ARG;
    cape_binding_init(out);
    const int rc = import_files(files, out);
    if(rc != CAPE_OK)
        cape_binding_free(out);
    return rc;
}

void cape_binding_broken(cape_binding_model* m, const char* why, int winlock_read, uint8_t winlock){
    if(!m)
        return;
    cape_binding_init(m);
    set_state(m, CAPE_BIND_BROKEN, "%s", why ? why : "");
    m->winlock_read = winlock_read;
    if(winlock_read)
        m->winlock = winlock;
}

static int copy_events(cape_binding_action* a, const cape_event* ev, unsigned n){
    cape_event* copy = malloc((size_t)(n ? n : 1) * sizeof(*copy));
    if(!copy)
        return CAPE_E_CAP;
    if(n)
        memcpy(copy, ev, (size_t)n * sizeof(*copy));
    free(a->events);
    a->events = copy;
    a->nevents = n;
    return CAPE_OK;
}

int cape_binding_copy(cape_binding_model* dst, const cape_binding_model* src){
    if(!dst || !src)
        return CAPE_E_ARG;
    memcpy(dst, src, sizeof(*dst));
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        dst->key[k].events = NULL;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++){
        if(src->key[k].kind != CAPE_BIND_MACRO)
            continue;
        if(copy_events(&dst->key[k], src->key[k].events, src->key[k].nevents) != CAPE_OK){
            cape_binding_free(dst);
            return CAPE_E_CAP;
        }
    }
    return CAPE_OK;
}

static void count(cape_binding_model* m){
    m->remaps = m->macros = 0;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++){
        m->remaps += m->key[k].kind == CAPE_BIND_REMAP;
        m->macros += m->key[k].kind == CAPE_BIND_MACRO;
    }
}

// An edit changes the action of one key: a file it shared is shared no longer by it (and maybe by nobody)
static void unshare(cape_binding_model* m, unsigned key){
    const char* name = m->key[key].file;
    if(!(m->key[key].flags & CAPE_BIND_F_SHARED))
        return;
    unsigned left = 0, last = 0;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        if(k != key && m->key[k].kind == CAPE_BIND_MACRO && (m->key[k].flags & CAPE_BIND_F_SHARED) && !strcmp(m->key[k].file, name)){
            left++;
            last = k;
        }
    if(left == 1)
        m->key[last].flags &= ~(unsigned)CAPE_BIND_F_SHARED;
}

static int editable(const cape_binding_model* m, unsigned key){
    return m && m->state == CAPE_BIND_OK && key < CAPE_BIND_K95_KEYS;
}

int cape_binding_set_native(cape_binding_model* m, unsigned key){
    if(!editable(m, key))
        return CAPE_E_ARG;
    unshare(m, key);
    clear_action(&m->key[key]);
    count(m);
    return CAPE_OK;
}

int cape_binding_set_remap(cape_binding_model* m, unsigned key, uint8_t dst){
    if(!editable(m, key))
        return CAPE_E_ARG;
    unshare(m, key);
    clear_action(&m->key[key]);
    m->key[key].kind = CAPE_BIND_REMAP;
    m->key[key].dst = dst;
    count(m);
    return CAPE_OK;
}

int cape_binding_set_macro(cape_binding_model* m, unsigned key, uint8_t subtype, uint8_t start, uint8_t run, uint8_t repeat,
                           const cape_event* events, unsigned nevents){
    if(!editable(m, key) || (!events && nevents))
        return CAPE_E_ARG;
    cape_binding_action a = {0};
    if(copy_events(&a, events, nevents) != CAPE_OK)
        return CAPE_E_CAP;
    unshare(m, key);
    clear_action(&m->key[key]);
    a.kind = CAPE_BIND_MACRO;
    a.subtype = subtype;
    a.start = start;
    a.run = run;
    a.repeat = repeat;
    a.flags = flags_of(subtype, start, run, repeat);
    m->key[key] = a;
    count(m);
    return CAPE_OK;
}

int cape_binding_action_equal(const cape_binding_action* a, const cape_binding_action* b){
    if(!a || !b || a->kind != b->kind)
        return 0;
    switch(a->kind){
    case CAPE_BIND_NATIVE:
        return 1;
    case CAPE_BIND_REMAP:
        return a->dst == b->dst;
    case CAPE_BIND_MACRO:
        return a->subtype == b->subtype && a->start == b->start && a->run == b->run && a->repeat == b->repeat
               && a->nevents == b->nevents && same_events(a->events, b->events, a->nevents);
    }
    return 0;
}

int cape_binding_equal(const cape_binding_model* a, const cape_binding_model* b){
    if(!a || !b || a->state != b->state)
        return 0;
    if(a->state != CAPE_BIND_OK)
        return a->state == CAPE_BIND_EMPTY;   // RAW and BROKEN say nothing that can be compared
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        if(!cape_binding_action_equal(&a->key[k], &b->key[k]))
            return 0;
    return 1;
}

// iCUE's Keystroke (subtype 01): up to three of Win, Ctrl, Alt, Shift, left ones only and in this order, then at most
// one other key; every key pressed in that order and released in the reverse one, a delay of 0 between two events
static const uint8_t shortcut_modifiers[] = { 0x3d, 0x3c, 0x3e, 0x30 };   // lwin, lctrl, lalt, lshift (key indices of the files)
static const uint8_t shortcut_right[] = { 0x44, 0x5b, 0x43, 0x5a };      // rwin, rctrl, ralt, rshift: written as the left ones

// The keys iCUE's GUI offers: its Keyboard picker, the US
// ANSI keys without the G keys, the profile, brightness and lock buttons; the media keys of its Media action; the 9 Language keys it
// writes on a real key (the other 12 it writes as 00, Esc)
static const uint8_t icue_keyboard[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,   // esc f1..f11
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,   // grave 1..9 0 minus
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,   // tab q w e r t y u i o p lbrace
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,   // caps a s d f g h j k l colon quote
    0x30, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b,         // lshift z x c v b n m comma dot slash
    0x3c, 0x3d, 0x3e, 0x40, 0x43, 0x44, 0x45,                                 // lctrl lwin lalt space ralt rwin rmenu
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x52, 0x54,         // f12 prtscn scroll pause ins home pgup rbrace bslash enter equal
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,               // bspace del end pgdn rshift rctrl up left down right
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x70, 0x71, 0x72,   // numlock numslash numstar numminus numplus numenter num7..9 num4..6
    0x73, 0x74, 0x75, 0x76, 0x77,                                             // num1..3 num0 numdot
};
static const uint8_t icue_media[] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x82, 0x83 };   // mute stop prev play next volup voldn
static const uint8_t icue_language[] = { 0x31, 0x3f, 0x41, 0x42, 0x51, 0x53, 0x55, 0x90, 0x91 };   // bslash_iso hanja hangul katahira hash ro yen muhenkan henkan

static int in_set(const uint8_t* set, size_t n, uint8_t k){
    for(size_t i = 0; i < n; i++)
        if(set[i] == k)
            return 1;
    return 0;
}

unsigned cape_binding_icue_sets(uint8_t key){
    unsigned sets = 0;
    if(in_set(icue_keyboard, sizeof(icue_keyboard), key))
        sets |= CAPE_BIND_ICUE_KEYBOARD;
    if(in_set(icue_media, sizeof(icue_media), key))
        sets |= CAPE_BIND_ICUE_MEDIA;
    if(in_set(icue_language, sizeof(icue_language), key))
        sets |= CAPE_BIND_ICUE_LANGUAGE;
    return sets;
}

static int modifier_rank(uint8_t k){
    for(int i = 0; i < 4; i++)
        if(shortcut_modifiers[i] == k || shortcut_right[i] == k)
            return i;
    return -1;
}

static const char* shortcut_outside(const cape_binding_action* a){
    const unsigned n = (a->nevents + 1) / 4;   // keys; more than four break one of the rules below
    if(a->nevents % 4 != 3)                    // also what keeps the reads below inside the events (j < 2n)
        return "not the shape of a shortcut (keys pressed, then released in the reverse order, a delay of 0 between two events)";
    for(unsigned i = 0; i < a->nevents; i++){
        const cape_event e = a->events[i];
        if(i % 2){
            if(cape_ev_kind(e) != CAPE_EV_DELAY || cape_ev_delay_ms(e) != 0)
                return "not the shape of a shortcut (a delay of 0 between two events)";
            continue;
        }
        const unsigned j = i / 2;   // the j-th key event: presses 0..n-1, then the releases in the reverse order
        const cape_event want = j < n ? cape_ev_press(cape_ev_key(a->events[2 * j])) : cape_ev_release(cape_ev_key(a->events[2 * (2 * n - 1 - j)]));
        if(e.b0 != want.b0 || e.b1 != want.b1)
            return "not the shape of a shortcut (keys pressed, then released in the reverse order)";
    }
    int last = -1;
    unsigned others = 0;
    for(unsigned j = 0; j < n; j++){
        const uint8_t k = cape_ev_key(a->events[2 * j]);
        const int rank = modifier_rank(k);
        if(rank >= 0 && shortcut_modifiers[rank] != k)
            return "a shortcut has the left Win, Ctrl, Alt and Shift only";
        if(rank < 0){
            others++;
            continue;
        }
        if(others || rank <= last)
            return "a shortcut presses Win, Ctrl, Alt, Shift in this order, then its key";
        last = rank;
    }
    if(others > 1)
        return "a shortcut has at most one key besides Win, Ctrl, Alt and Shift";
    if(n - others > 3)
        return "a shortcut has at most three of Win, Ctrl, Alt and Shift";
    return NULL;
}

// Every key pressed is released, none twice: what makes an action safe to run (the first problem, or NULL)
static const char* unpaired(const cape_binding_action* a, char* buf, size_t cap){
    uint8_t down[CAPE_BIND_K95_KEYS] = {0};
    for(unsigned i = 0; i < a->nevents; i++){
        const int kind = cape_ev_kind(a->events[i]);
        const uint8_t k = cape_ev_key(a->events[i]);
        if(kind == CAPE_EV_PRESS){
            if(down[k]){
                say(buf, cap, "event %u presses key %02x again before releasing it", i, k);
                return buf;
            }
            down[k] = 1;
        } else if(kind == CAPE_EV_RELEASE){
            if(!down[k]){
                say(buf, cap, "event %u releases key %02x, which is not pressed", i, k);
                return buf;
            }
            down[k] = 0;
        }
    }
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        if(down[k]){
            say(buf, cap, "key %02x is pressed and never released", k);
            return buf;
        }
    return NULL;
}

static int is_key_event(cape_event e){
    const int kind = cape_ev_kind(e);
    return kind == CAPE_EV_PRESS || kind == CAPE_EV_RELEASE;
}

unsigned cape_binding_macro_rows(const cape_event* ev, unsigned n){
    // iCUE puts a delay of 0 between two key events by itself: such a delay is no row of its editor
    unsigned rows = 0;
    for(unsigned i = 0; i < n; i++){
        const int zero_between = i > 0 && i + 1 < n && cape_ev_kind(ev[i]) == CAPE_EV_DELAY && cape_ev_delay_ms(ev[i]) == 0
                                 && is_key_event(ev[i - 1]) && is_key_event(ev[i + 1]);
        rows += !zero_between;
    }
    return rows;
}

// A delay in the form iCUE writes: 2 bytes below 8192 ms, 4 from there
static int icue_delay(cape_event e){
    const int kind = cape_ev_kind(e);
    return kind == CAPE_EV_DELAY || (kind == CAPE_EV_LONG && cape_ev_delay_ms(e) > CAPE_DELAY_SHORT_MAX);
}

// iCUE's Macro of a hardware profile: keys and delays of at most 4095 ms, constant or random, at most 256 rows
static const char* macro_outside(const cape_binding_action* a, char* buf, size_t cap){
    for(unsigned i = 0; i < a->nevents; i++){
        const cape_event e = a->events[i];
        const int kind = cape_ev_kind(e);
        if(kind == CAPE_EV_DELAY && cape_ev_delay_ms(e) > CAPE_BIND_MACRO_DELAY_MAX){
            say(buf, cap, "event %u: a delay of a macro is at most %u ms", i, (unsigned)CAPE_BIND_MACRO_DELAY_MAX);
            return buf;
        }
        if(kind == CAPE_EV_LONG || kind == CAPE_EV_ODD_SHORT){
            say(buf, cap, "event %u: a delay of a macro is at most %u ms, in 2 bytes", i, (unsigned)CAPE_BIND_MACRO_DELAY_MAX);
            return buf;
        }
        if(kind == CAPE_EV_RANDOM && cape_ev_random_min(e) > cape_ev_random_max(e)){
            say(buf, cap, "event %u: a random delay from %u to %u ms", i, cape_ev_random_min(e), cape_ev_random_max(e));
            return buf;
        }
    }
    const unsigned rows = cape_binding_macro_rows(a->events, a->nevents);
    if(rows > CAPE_BIND_MACRO_ROWS_MAX){
        say(buf, cap, "a macro has at most %u rows, this one %u", (unsigned)CAPE_BIND_MACRO_ROWS_MAX, rows);
        return buf;
    }
    return NULL;
}

// iCUE's Text: keys, and delays of at most 99999 ms in iCUE's form, never a random one
static const char* text_outside(const cape_binding_action* a, char* buf, size_t cap){
    for(unsigned i = 0; i < a->nevents; i++){
        const cape_event e = a->events[i];
        if(is_key_event(e))
            continue;
        if(!icue_delay(e)){
            say(buf, cap, "event %u: a delay of a text is constant, in the form iCUE writes", i);
            return buf;
        }
        if(cape_ev_delay_ms(e) > CAPE_BIND_TEXT_DELAY_MAX){
            say(buf, cap, "event %u: a delay of a text is at most %u ms", i, (unsigned)CAPE_BIND_TEXT_DELAY_MAX);
            return buf;
        }
    }
    return NULL;
}

// 'Imitate holding key' (corsair-protocol formats/cape/macro.md#imitate-holding-key): key k pressed, a delay, k released; on press
// (run 1, repeat 1) holds it for 0.1 to 16 777.2 s in steps of 0.1 s, or c0 ff ff ff, iCUE's clamp of longer times; the toggle mode
// (run 84, repeat 0) has a delay of 0. k is a key of iCUE's keyboard, not a media key (a modifier is one). NULL if the action is this
static const char* imitate_outside(const cape_binding_action* a, char* buf, size_t cap){
    // (the same key in both: the pairing of presses and releases checks that)
    if(a->nevents != 3 || cape_ev_kind(a->events[0]) != CAPE_EV_PRESS || cape_ev_kind(a->events[2]) != CAPE_EV_RELEASE)
        return "not the shape of 'Imitate holding key' (a key pressed, a delay, the key released)";
    const uint8_t k = cape_ev_key(a->events[0]);
    if(!(cape_binding_icue_sets(k) & CAPE_BIND_ICUE_KEYBOARD)){
        say(buf, cap, "'Imitate holding key' holds a key of iCUE's keyboard, not %02x", k);
        return buf;
    }
    const cape_event d = a->events[1];
    if(a->run == 0x84){
        if(cape_ev_kind(d) != CAPE_EV_DELAY || cape_ev_delay_ms(d) != 0)
            return "'Imitate holding key' in its toggle mode has a delay of 0";
        return NULL;
    }
    const cape_event clamp = { 0xc0, 0xff, 0xff, 0xff };
    if(cape_ev_equal(d, clamp))
        return NULL;
    // From CAPE_BIND_IMITATE_MIN_MS to CAPE_BIND_IMITATE_MAX_MS in steps of 100 ms: a delay of 0 makes this a Keystroke, and
    // 16 777 200 is the largest multiple of 100 that fits in 24 bits, so the step says the whole range
    const unsigned ms = cape_ev_delay_ms(d);
    if(!icue_delay(d) || ms % 100){
        say(buf, cap, "'Imitate holding key' holds a key 0.1 to 16777.2 s, in steps of 0.1 s, in the form iCUE writes");
        return buf;
    }
    return NULL;
}

// Whether a new or changed macro action is one iCUE writes, and safe to run: every key pressed is released, none twice
static int check_macro(const cape_binding_action* a, unsigned key, char* why, size_t whycap){
    char buf[CAPE_BIND_WHY];
    const int values = (a->subtype == CAPE_MACRO_MACRO && ((a->start == 0x00 && (a->run == 1 || a->run == 3 || a->run == 4)) || (a->start == 0x11 && a->run == 1))
                        && a->repeat == 1)
                       || ((a->subtype == CAPE_MACRO_SHORTCUT || a->subtype == CAPE_MACRO_TEXT) && a->start == 0x00 && a->run == 1 && a->repeat == 1)
                       || (a->subtype == CAPE_MACRO_SHORTCUT && a->start == 0x00 && a->run == 0x84 && a->repeat == 0);
    if(!values){
        say(why, whycap, "key %02x: subtype %u, start %02x, run %u, repeat %u is not an action iCUE writes", key, a->subtype, a->start,
            a->run, a->repeat);
        return CAPE_E_RANGE;
    }
    if(a->nevents < 1 || a->nevents > CAPE_BIND_EDIT_EVENTS_MAX){
        say(why, whycap, "key %02x: %u events (1 to %u)", key, a->nevents, (unsigned)CAPE_BIND_EDIT_EVENTS_MAX);
        return CAPE_E_RANGE;
    }
    const char* bad = events_outside(a->events, a->nevents);
    if(!bad && a->subtype == CAPE_MACRO_MACRO)
        bad = macro_outside(a, buf, sizeof(buf));
    else if(!bad && a->subtype == CAPE_MACRO_TEXT)
        bad = text_outside(a, buf, sizeof(buf));
    else if(!bad && a->subtype == CAPE_MACRO_SHORTCUT){
        // 'Imitate holding key' when it has a run 84 or a delay that is not 0 between its two events; else a Keystroke
        const int imitate = a->run == 0x84 || (a->nevents == 3 && !(cape_ev_kind(a->events[1]) == CAPE_EV_DELAY && cape_ev_delay_ms(a->events[1]) == 0));
        bad = imitate ? imitate_outside(a, buf, sizeof(buf)) : shortcut_outside(a);
        if(!bad && !imitate)
            for(unsigned i = 0; i < a->nevents; i += 2){
                const uint8_t k = cape_ev_key(a->events[i]);
                if(modifier_rank(k) < 0 && !(cape_binding_icue_sets(k) & CAPE_BIND_ICUE_KEYBOARD)){
                    say(buf, sizeof(buf), "a shortcut has a key of iCUE's keyboard, not %02x", k);
                    bad = buf;
                    break;
                }
            }
    }
    if(!bad)
        bad = unpaired(a, buf, sizeof(buf));
    if(bad){
        say(why, whycap, "key %02x: %s", key, bad);
        return CAPE_E_RANGE;
    }
    return CAPE_OK;
}

int cape_binding_check(const cape_binding_model* edit, const cape_binding_model* base, char* why, size_t whycap){
    say(why, whycap, "%s", "");
    if(!edit || edit->state != CAPE_BIND_OK){
        say(why, whycap, "the bindings to write are not a model");
        return CAPE_E_ARG;
    }
    const int keep = base && base->state == CAPE_BIND_OK;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++){
        const cape_binding_action* a = &edit->key[k];
        if(keep && cape_binding_action_equal(a, &base->key[k]))
            continue;
        if(a->kind == CAPE_BIND_REMAP && a->dst >= CAPE_BIND_K95_KEYS && !is_mouse(a->dst)){
            say(why, whycap, "key %02x: remap to %02x, which is neither a key nor a mouse button", k, a->dst);
            return CAPE_E_RANGE;
        }
        if(a->kind == CAPE_BIND_REMAP && !is_mouse(a->dst) && !cape_binding_icue_sets(a->dst)){
            say(why, whycap, "key %02x: remap to %02x, which iCUE does not offer", k, a->dst);
            return CAPE_E_RANGE;
        }
        if(a->kind == CAPE_BIND_MACRO){
            const int rc = check_macro(a, k, why, whycap);
            if(rc != CAPE_OK)
                return rc;
        }
    }
    return CAPE_OK;
}

// Adds a file at the end of out; the caller rolls back to its n on failure
static int add(cape_slot_files* out, const char* name, const uint8_t* data, int len){
    if(len < 0)
        return len;
    return cape_slot_files_add(out, name, data, (size_t)len);
}

int cape_binding_build(const cape_binding_model* m, cape_slot_files* out){
    if(!m || !out || m->state != CAPE_BIND_OK)
        return CAPE_E_ARG;
    unsigned nmacro = 0;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        nmacro += m->key[k].kind == CAPE_BIND_MACRO;
    if(out->n + 2 + nmacro > CAPE_SLOT_FILES_MAX)
        return CAPE_E_CAP;
    // Room for the largest file: the DAT of every key, or the largest macro
    size_t room = CAPE_DAT_HEADER + (size_t)CAPE_BIND_K95_KEYS * CAPE_DAT_ENTRY_SIZE;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++){
        if(m->key[k].kind != CAPE_BIND_MACRO)
            continue;
        const size_t len = (size_t)CAPE_MACRO_HEADER + cape_macro_bytes(m->key[k].events, m->key[k].nevents);
        if(len > CAPE_DAT_SIZE_MAX)
            return CAPE_E_RANGE;
        if(len > room)
            room = len;
    }

    const size_t n0 = out->n;
    cape_map_entry map[CAPE_BIND_K95_KEYS];
    cape_dat_entry dat[CAPE_BIND_K95_KEYS];
    unsigned i = 0;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
        if(m->key[k].kind == CAPE_BIND_REMAP && k != CAPE_BIND_KEY_PROFSWITCH)
            map[i++] = (cape_map_entry){ (uint8_t)k, m->key[k].dst, CAPE_MAP_FLAG_REMAP };
    if(m->key[CAPE_BIND_KEY_PROFSWITCH].kind == CAPE_BIND_REMAP)
        map[i++] = (cape_map_entry){ CAPE_BIND_KEY_PROFSWITCH, m->key[CAPE_BIND_KEY_PROFSWITCH].dst, CAPE_MAP_FLAG_REMAP };

    int rc = CAPE_OK;
    uint8_t* buf = malloc(room);
    if(!buf)
        return CAPE_E_CAP;
    unsigned j = 0;
    for(unsigned k = 0; k < CAPE_BIND_K95_KEYS && rc == CAPE_OK; k++){
        const cape_binding_action* a = &m->key[k];
        if(a->kind != CAPE_BIND_MACRO)
            continue;
        char name[5];
        cape_macro_filename(j, name);
        const cape_macro_hdr hdr = { a->subtype, { 0, 0, 0 } };
        const int len = cape_macro_build(&hdr, a->events, a->nevents, buf, room);
        rc = add(out, name, buf, len);
        memset(&dat[j], 0, sizeof(dat[j]));
        dat[j].key = (uint8_t)k;
        memcpy(dat[j].name, name, 4);
        dat[j].size = len > 0 ? (uint32_t)len : 0;
        dat[j].start = a->start;
        dat[j].runtype = a->run;
        dat[j].repeat = a->repeat;
        j++;
    }
    if(rc == CAPE_OK)
        rc = add(out, CAPE_FILE_DAT, buf, cape_dat_build_ex(m->winlock, dat, j, buf, room));
    if(rc == CAPE_OK)
        rc = add(out, CAPE_FILE_MAP, buf, cape_map_build(map, i, buf, room));
    free(buf);
    if(rc != CAPE_OK){
        while(out->n > n0){
            out->n--;
            free(out->file[out->n].data);
            memset(&out->file[out->n], 0, sizeof(out->file[out->n]));
        }
    }
    return rc;
}

const char* cape_binding_state_name(cape_binding_state s){
    switch(s){
    case CAPE_BIND_EMPTY:
        return "empty";
    case CAPE_BIND_OK:
        return "ok";
    case CAPE_BIND_RAW:
        return "raw";
    case CAPE_BIND_BROKEN:
        return "broken";
    }
    return "?";
}
