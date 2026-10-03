#include "cape_stage.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The order of the words of a transaction (cape_hwslot.h, section 2)
enum { PH_BEGIN = 0, PH_NAME, PH_LIGHT, PH_RGB, PH_WL, PH_IND, PH_BIND, PH_MAP, PH_ACT, PH_END };

// A sanity bound on what one transaction may carry (the limits of an edit are cape_binding_check's, at check:): 64 macro files as
// large as the daemon reads one, 16 MiB, far above what a slot holds (the flash has 2038 sectors of 4 KiB)
#define STAGE_EVENTS_MAX   (CAPE_MAX_FILE_SIZE / CAPE_EVENT_SIZE)
#define STAGE_BYTES_MAX    ((size_t)64 * CAPE_MAX_FILE_SIZE)

static void txn_clear(cape_stage_txn* t){
    cape_binding_free(&t->model);
    free(t->rgb);
    free(t->act_ev);
    memset(t, 0, sizeof(*t));
    t->act_key = -1;
}

void cape_stage_init(cape_stage* st){
    if(!st)
        return;
    memset(st, 0, sizeof(*st));
    for(unsigned i = 0; i < CAPE_STAGE_SLOTS; i++)
        st->slot[i].act_key = -1;
}

void cape_stage_free(cape_stage* st){
    if(!st)
        return;
    for(unsigned i = 0; i < CAPE_STAGE_SLOTS; i++)
        txn_clear(&st->slot[i]);
}

void cape_stage_end_slot(cape_stage* st, unsigned slot){
    if(st && slot < CAPE_STAGE_SLOTS)
        txn_clear(&st->slot[slot]);
}

void cape_stage_end_owner(cape_stage* st, int owner){
    if(!st)
        return;
    for(unsigned i = 0; i < CAPE_STAGE_SLOTS; i++)
        if(st->slot[i].open && st->slot[i].owner == owner)
            txn_clear(&st->slot[i]);
}

static void fail(cape_stage_txn* t, const char* fmt, ...){
    if(t->failed)
        return;   // the first thing wrong is the one to tell
    t->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->why, sizeof(t->why), fmt, ap);
    va_end(ap);
}

// Splits a word at its first `max - 1` colons: the last field is the rest. Returns the number of fields.
static unsigned fields(const char* w, size_t len, const char** f, size_t* fl, unsigned max){
    unsigned n = 0;
    size_t start = 0;
    for(size_t i = 0; i <= len && n < max; i++){
        if(i == len || (w[i] == ':' && n < max - 1)){
            f[n] = w + start;
            fl[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    return n;
}

// Lowercase hex of exactly n digits (n <= 8)
static int hex_exact(const char* p, size_t len, size_t n, uint32_t* v){
    if(len != n)
        return 0;
    *v = 0;
    for(size_t i = 0; i < n; i++){
        const char c = p[i];
        int d;
        if(c >= '0' && c <= '9')
            d = c - '0';
        else if(c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return 0;
        *v = *v << 4 | (uint32_t)d;
    }
    return 1;
}

// A decimal number without leading zeros, at most max
static int dec(const char* p, size_t len, unsigned max, unsigned* v){
    if(len == 0 || len > 10 || (len > 1 && p[0] == '0'))
        return 0;
    unsigned long long x = 0;
    for(size_t i = 0; i < len; i++){
        if(p[i] < '0' || p[i] > '9')
            return 0;
        x = x * 10 + (unsigned)(p[i] - '0');
    }
    if(x > max)
        return 0;
    *v = (unsigned)x;
    return 1;
}

static int is(const char* p, size_t len, const char* s){
    return strlen(s) == len && !memcmp(p, s, len);
}

// The transaction of a word, if it is open for owner
static cape_stage_txn* find(cape_stage* st, int owner, uint32_t txn){
    for(unsigned i = 0; i < CAPE_STAGE_SLOTS; i++)
        if(st->slot[i].open && st->slot[i].txn == txn && st->slot[i].owner == owner)
            return &st->slot[i];
    return NULL;
}

static void phase(cape_stage_txn* t, int ph, const char* word){
    if(t->phase > ph)
        fail(t, "%s comes too late", word);
    else if(t->complete || t->phase == PH_END)
        fail(t, "%s after end", word);
    t->phase = ph;
}

// The action whose events have all come goes into the model
static void act_done(cape_stage_txn* t){
    if(cape_binding_set_macro(&t->model, (unsigned)t->act_key, t->act_sub, t->act_start, t->act_run, t->act_rep, t->act_ev, t->act_need)
       != CAPE_OK)
        fail(t, "out of memory");
    free(t->act_ev);
    t->act_ev = NULL;
    t->act_key = -1;
}

static void need_no_act(cape_stage_txn* t, const char* word){
    if(t->act_key >= 0)
        fail(t, "%s before the events of key %02x are all there", word, t->act_key);
}

static void w_name(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_NAME, "name");
    if(t->failed)
        return;
    unsigned i = 0, n = 0;
    const char* slash = nf == 4 ? memchr(f[2], '/', fl[2]) : NULL;
    if(!slash || !dec(f[2], (size_t)(slash - f[2]), CAPE_HWSLOT_NAME_WORDS, &i)
       || !dec(slash + 1, fl[2] - (size_t)(slash - f[2]) - 1, CAPE_HWSLOT_NAME_WORDS, &n) || i < 1 || n < 1 || i > n){
        fail(t, "a malformed name word");
        return;
    }
    if(i != t->name_next + 1 || (t->name_total && n != t->name_total)){
        fail(t, "name word %u/%u out of order", i, n);
        return;
    }
    t->name_total = n;
    t->name_next = i;
    const int got = cape_hwslot_decode(f[3], fl[3], t->name_utf8 + t->name_len, sizeof(t->name_utf8) - t->name_len);
    if(got < 0){
        fail(t, got == CAPE_E_CAP ? "the name is too long" : "a name that is not percent-encoded as it should be");
        return;
    }
    t->name_len += (size_t)got;
    if(i < n)
        return;
    uint16_t units[CAPE_INFO_NAME_MAX + 1];
    const int u = cape_hwslot_utf16(t->name_utf8, t->name_len, units, CAPE_INFO_NAME_MAX + 1);
    if(u == CAPE_E_CAP || u > CAPE_INFO_NAME_MAX)
        fail(t, "the name is longer than %d UTF-16 units", CAPE_INFO_NAME_MAX);
    else if(u <= 0)
        fail(t, u == 0 ? "an empty name" : "a name that is not UTF-8");
    else {
        memcpy(t->name, units, (size_t)u * sizeof(*units));
        t->name_units = (size_t)u;
        t->has_name = 1;
    }
}

static int led_char(char c){
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static void w_rgb(cape_stage_txn* t, const char* p, size_t len){
    phase(t, PH_RGB, "rgb");
    if(t->failed)
        return;
    if(t->light != CAPE_STAGE_LIGHT_PIC){
        fail(t, "rgb without light:pic");
        return;
    }
    size_t i = 0;
    for(;;){
        const char* item = p + i;
        const char* comma = memchr(item, ',', len - i);
        const size_t il = comma ? (size_t)(comma - item) : len - i;
        const char* eq = memchr(item, '=', il);
        uint32_t rgb;
        const size_t nl = eq ? (size_t)(eq - item) : 0;
        if(!eq || nl < 1 || nl >= CAPE_STAGE_LED_NAME || !hex_exact(eq + 1, il - nl - 1, 6, &rgb)){
            fail(t, "a malformed rgb word");
            return;
        }
        for(size_t k = 0; k < nl; k++)
            if(!led_char(item[k])){
                fail(t, "a malformed LED name");
                return;
            }
        cape_stage_rgb e = {{0}, {0}};
        memcpy(e.led, item, nl);
        e.rgb[0] = (uint8_t)(rgb >> 16);
        e.rgb[1] = (uint8_t)(rgb >> 8);
        e.rgb[2] = (uint8_t)rgb;
        if(t->nrgb && strcmp(t->rgb[t->nrgb - 1].led, e.led) >= 0){
            fail(t, "LED %s out of order or twice", e.led);
            return;
        }
        if(!(e.rgb[0] | e.rgb[1] | e.rgb[2])){
            fail(t, "LED %s is black: a black LED is not listed", e.led);
            return;
        }
        if(t->nrgb == t->rgb_cap){
            const size_t cap = t->rgb_cap ? t->rgb_cap * 2 : 64;
            cape_stage_rgb* r = cap <= 4096 ? realloc(t->rgb, cap * sizeof(*r)) : NULL;
            if(!r){
                fail(t, "too many LEDs");
                return;
            }
            t->rgb = r;
            t->rgb_cap = cap;
        }
        t->rgb[t->nrgb++] = e;
        if(!comma)
            return;
        i += il + 1;
    }
}

// wl:<txn>:<hh>, the Win Lock options to write (Performance)
static void w_wl(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_WL, "wl");
    if(t->failed)
        return;
    uint32_t v;
    if(t->has_wl)
        fail(t, "wl twice");
    else if(nf != 3 || !hex_exact(f[2], fl[2], 2, &v))
        fail(t, "a malformed wl word");
    else {
        t->wl = (uint8_t)v;
        t->has_wl = 1;
    }
}

// ind:<txn>:<24 hex>, the indicator colours to write (Performance): profile, brightness, lock on, lock off
static void w_ind(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_IND, "ind");
    if(t->failed)
        return;
    if(t->has_ind){
        fail(t, "ind twice");
        return;
    }
    if(nf != 3 || fl[2] != 2 * sizeof(t->ind)){
        fail(t, "a malformed ind word");
        return;
    }
    for(size_t i = 0; i < sizeof(t->ind); i++){
        uint32_t v;
        if(!hex_exact(f[2] + 2 * i, 2, 2, &v)){
            fail(t, "a malformed ind word");
            return;
        }
        t->ind[i] = (uint8_t)v;
    }
    t->has_ind = 1;
}

static void w_bind(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_BIND, "bind");
    if(t->failed)
        return;
    if(t->bind != CAPE_STAGE_BIND_UNSET){
        fail(t, "bind twice");
        return;
    }
    if(nf == 3 && is(f[2], fl[2], "keep")){
        t->bind = CAPE_STAGE_BIND_KEEP;
        return;
    }
    // bind:<txn>:model[:recreate] (the Win Lock options are the wl: word's, not the bindings')
    const char* rest = f[2];
    const size_t rl = fl[2];
    const char* g[2];
    size_t gl[2];
    const unsigned ng = nf == 3 ? fields(rest, rl, g, gl, 2) : 0;
    if(ng < 1 || !is(g[0], gl[0], "model") || (ng == 2 && !is(g[1], gl[1], "recreate"))){
        fail(t, "a malformed bind word");
        return;
    }
    t->bind = CAPE_STAGE_BIND_MODEL;
    t->recreate = ng == 2;
    cape_binding_init(&t->model);
}

static int key_of(const char* p, size_t len, uint32_t* k){
    return hex_exact(p, len, 2, k) && *k < CAPE_BIND_K95_KEYS;
}

static void w_map(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_MAP, "map");
    if(t->failed)
        return;
    uint32_t k, d;
    if(t->bind != CAPE_STAGE_BIND_MODEL){
        fail(t, "map without bind:model");
        return;
    }
    if(nf != 4 || !key_of(f[2], fl[2], &k) || !hex_exact(f[3], fl[3], 2, &d)){
        fail(t, "a malformed map word");
        return;
    }
    if(t->last_map && (int)k < t->last_map){
        fail(t, "map of key %02x out of order", (unsigned)k);
        return;
    }
    t->last_map = (int)k + 1;
    if(t->model.key[k].kind != CAPE_BIND_NATIVE){
        fail(t, "key %02x twice", (unsigned)k);
        return;
    }
    cape_binding_set_remap(&t->model, k, (uint8_t)d);
}

static void w_act(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    need_no_act(t, "act");
    phase(t, PH_ACT, "act");
    if(t->failed)
        return;   // (and an action whose events are awaited keeps its buffer, freed with the transaction)
    uint32_t k, sub, start, run, rep;
    unsigned nev;
    if(t->bind != CAPE_STAGE_BIND_MODEL){
        fail(t, "act without bind:model");
        return;
    }
    if(nf != 8 || !key_of(f[2], fl[2], &k) || !hex_exact(f[3], fl[3], 2, &sub) || !hex_exact(f[4], fl[4], 2, &start)
       || !hex_exact(f[5], fl[5], 2, &run) || !hex_exact(f[6], fl[6], 2, &rep) || !dec(f[7], fl[7], STAGE_EVENTS_MAX, &nev)){
        fail(t, "a malformed act word");
        return;
    }
    if(t->last_act && (int)k < t->last_act){
        fail(t, "act of key %02x out of order", (unsigned)k);
        return;
    }
    t->last_act = (int)k + 1;
    if(t->model.key[k].kind != CAPE_BIND_NATIVE){
        fail(t, "key %02x twice", (unsigned)k);
        return;
    }
    t->event_bytes += (size_t)nev * CAPE_EVENT_SIZE;
    if(t->event_bytes > STAGE_BYTES_MAX){
        fail(t, "more macro events than a slot can hold");
        return;
    }
    t->act_ev = calloc((size_t)(nev ? nev : 1), sizeof(*t->act_ev));   // zeroed: the bytes past a 2-byte event stay zero
    if(!t->act_ev){
        fail(t, "out of memory");
        return;
    }
    t->act_key = (int)k;
    t->act_sub = (uint8_t)sub;
    t->act_start = (uint8_t)start;
    t->act_run = (uint8_t)run;
    t->act_rep = (uint8_t)rep;
    t->act_need = nev;
    t->act_have = 0;
    t->act_four_next = 0;
    if(nev == 0)
        act_done(t);
}

static void w_ev(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    phase(t, PH_ACT, "ev");
    if(t->failed)
        return;
    uint32_t k;
    unsigned off;
    if(nf != 5 || !key_of(f[2], fl[2], &k) || !dec(f[3], fl[3], STAGE_EVENTS_MAX, &off) || fl[4] == 0 || fl[4] % 4){
        fail(t, "a malformed ev word");
        return;
    }
    if(t->act_key != (int)k || off != t->act_have){
        fail(t, "ev of key %02x at %u is not the one awaited", (unsigned)k, off);
        return;
    }
    // The events as in the file, 4 or 8 hex digits each by the first byte (cape_hwslot.h); as many whole ones as fit in
    // CAPE_HWSLOT_EVENT_BYTES, so a word that is not the last holds 126 or 128 bytes, 126 only before an event of 4
    unsigned n = 0;
    size_t bytes = 0;
    for(size_t at = 0; at < fl[4];){
        uint32_t b0, rest;
        if(!hex_exact(f[4] + at, 2, 2, &b0)){
            fail(t, "a malformed ev word");
            return;
        }
        const unsigned size = cape_ev_size_of((uint8_t)b0);
        if(at + 2 * size > fl[4] || !hex_exact(f[4] + at + 2, 2 * size - 2, 2 * size - 2, &rest)){
            fail(t, "a malformed ev word");
            return;
        }
        if(n == 0 && t->act_four_next && size != 4){
            fail(t, "ev of key %02x at %u is not cut where it should be", (unsigned)k, off);
            return;
        }
        if(t->act_have + n >= t->act_need){
            fail(t, "ev of key %02x at %u is not the one awaited", (unsigned)k, off);
            return;
        }
        cape_event e = { (uint8_t)b0, (uint8_t)(size == 4 ? rest >> 16 : rest), (uint8_t)(size == 4 ? rest >> 8 : 0),
                         (uint8_t)(size == 4 ? rest : 0) };
        t->act_ev[t->act_have + n] = e;
        n++;
        bytes += size;
        at += 2 * size;
    }
    const int last = t->act_have + n == t->act_need;
    if(bytes > CAPE_HWSLOT_EVENT_BYTES || (!last && bytes < CAPE_HWSLOT_EVENT_BYTES - 2)){
        fail(t, "ev of key %02x at %u is not cut where it should be", (unsigned)k, off);
        return;
    }
    t->act_four_next = !last && bytes == CAPE_HWSLOT_EVENT_BYTES - 2;
    // act: counted 2 bytes for each event; the events of 4 bytes add the rest
    t->event_bytes += bytes - (size_t)n * CAPE_EVENT_SIZE;
    if(t->event_bytes > STAGE_BYTES_MAX){
        fail(t, "more macro events than a slot can hold");
        return;
    }
    t->act_have += n;
    if(last)
        act_done(t);
}

static void w_end(cape_stage_txn* t, const char** f, const size_t* fl, unsigned nf){
    unsigned n;
    need_no_act(t, "end");
    phase(t, PH_END, "end");
    if(nf != 3 || !dec(f[2], fl[2], 0xffffffffu, &n)){
        fail(t, "a malformed end word");
        return;
    }
    if(t->name_next != t->name_total)
        fail(t, "the name is not all there");
    if(t->light == CAPE_STAGE_LIGHT_UNSET)
        fail(t, "no light word");
    if(t->bind == CAPE_STAGE_BIND_UNSET)
        fail(t, "no bind word");
    if(n != t->words)
        fail(t, "end says %u words, %u came", n, t->words);
    if(!t->failed)
        t->complete = 1;
}

cape_stage_request cape_stage_word(cape_stage* st, int owner, const char* word, size_t len, unsigned* slot, uint32_t* txn){
    const char* f[8];
    size_t fl[8];
    unsigned s = 0;
    uint32_t id = 0;
    if(!st || !word || owner < 1 || owner >= CAPE_STAGE_OWNERS || len > CAPE_HWSLOT_WORD_MAX)
        return CAPE_STAGE_MALFORMED;
    const unsigned nf = fields(word, len, f, fl, 8);
    if(nf < 2 || !hex_exact(f[1], fl[1], 8, &id) || !id)
        return CAPE_STAGE_MALFORMED;

    // check:, save:, abort: name their slot, so that an answer can always be given
    const int req = is(f[0], fl[0], "check") ? CAPE_STAGE_CHECK : is(f[0], fl[0], "save") ? CAPE_STAGE_SAVE
                  : is(f[0], fl[0], "abort") ? CAPE_STAGE_ABORT : CAPE_STAGE_WORD;
    if(req != CAPE_STAGE_WORD){
        if(nf != 3 || !dec(f[2], fl[2], CAPE_STAGE_SLOTS, &s) || s < 1)
            return CAPE_STAGE_MALFORMED;
        if(slot)
            *slot = s - 1;
        if(txn)
            *txn = id;
        cape_stage_txn* t = &st->slot[s - 1];
        if(req == CAPE_STAGE_ABORT && t->open && t->txn == id && t->owner == owner)
            txn_clear(t);
        return (cape_stage_request)req;
    }

    if(is(f[0], fl[0], "begin")){
        // begin:<txn>:<m>:<base>, and the base may hold a colon
        const char* g[4];
        size_t gl[4];
        uint8_t base[CAPE_SLOTID_SIZE];
        if(fields(word, len, g, gl, 4) != 4 || !dec(g[2], gl[2], CAPE_STAGE_SLOTS, &s) || s < 1)
            return CAPE_STAGE_MALFORMED;
        const int kind = cape_hwslot_parse_id(g[3], gl[3], base);
        if(kind < 0)
            return CAPE_STAGE_MALFORMED;
        for(unsigned i = 0; i < CAPE_STAGE_SLOTS; i++)
            if(st->slot[i].open && (i == s - 1 || st->slot[i].txn == id))
                txn_clear(&st->slot[i]);
        cape_stage_txn* t = &st->slot[s - 1];
        t->open = 1;
        t->txn = id;
        t->owner = owner;
        t->base_empty = kind == 0;
        memcpy(t->base, base, sizeof(base));
        t->words = 1;
        t->phase = PH_BEGIN;
        return CAPE_STAGE_WORD;
    }

    cape_stage_txn* t = find(st, owner, id);
    if(!t){
        st->ignored++;
        return CAPE_STAGE_WORD;
    }
    if(t->failed){
        t->words++;
        return CAPE_STAGE_WORD;
    }
    if(is(f[0], fl[0], "end")){
        w_end(t, f, fl, nf);
        return CAPE_STAGE_WORD;
    }
    t->words++;
    if(t->complete || t->phase == PH_END)
        fail(t, "%.*s after end", (int)fl[0], f[0]);
    else if(is(f[0], fl[0], "name"))
        w_name(t, f, fl, fields(word, len, f, fl, 4));
    else if(is(f[0], fl[0], "light")){
        phase(t, PH_LIGHT, "light");
        if(!t->failed){
            if(t->light != CAPE_STAGE_LIGHT_UNSET)
                fail(t, "light twice");
            else if(nf == 3 && is(f[2], fl[2], "keep"))
                t->light = CAPE_STAGE_LIGHT_KEEP;
            else if(nf == 3 && is(f[2], fl[2], "pic"))
                t->light = CAPE_STAGE_LIGHT_PIC;
            else
                fail(t, "a malformed light word");
        }
    } else if(is(f[0], fl[0], "rgb")){
        const unsigned n3 = fields(word, len, f, fl, 3);
        if(n3 != 3)
            fail(t, "a malformed rgb word");
        else
            w_rgb(t, f[2], fl[2]);
    } else if(is(f[0], fl[0], "wl"))
        w_wl(t, f, fl, fields(word, len, f, fl, 3));
    else if(is(f[0], fl[0], "ind"))
        w_ind(t, f, fl, fields(word, len, f, fl, 3));
    else if(is(f[0], fl[0], "bind"))
        w_bind(t, f, fl, fields(word, len, f, fl, 3));
    else if(is(f[0], fl[0], "map"))
        w_map(t, f, fl, nf);
    else if(is(f[0], fl[0], "act"))
        w_act(t, f, fl, nf);
    else if(is(f[0], fl[0], "ev"))
        w_ev(t, f, fl, nf);
    else
        fail(t, "an unknown word %.*s", (int)fl[0], f[0]);
    return CAPE_STAGE_WORD;
}

cape_stage_txn* cape_stage_get(cape_stage* st, int owner, unsigned slot, uint32_t txn, char* why, size_t cap){
    const char* msg = NULL;
    cape_stage_txn* t = st && slot < CAPE_STAGE_SLOTS ? &st->slot[slot] : NULL;
    if(!t || !t->open || t->txn != txn || t->owner != owner)
        msg = "the transaction is not open for this node and slot";
    else if(t->failed)
        msg = t->why;
    else if(!t->complete)
        msg = "the transaction is not complete";
    if(msg){
        if(why && cap)
            snprintf(why, cap, "%s", msg);
        return NULL;
    }
    return t;
}
