#include "cape_hwbind.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The words as they are made: one buffer, each word followed by a NUL
typedef struct {
    char* buf;
    size_t len, cap;
    size_t* off;
    size_t n, ncap;
    int err;
} words;

static void add(words* w, const char* fmt, ...){
    if(w->err)
        return;
    char tmp[CAPE_HWSLOT_WORD_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(len < 0 || (size_t)len >= sizeof(tmp)){
        w->err = CAPE_E_RANGE;
        return;
    }
    if(w->len + (size_t)len + 1 > w->cap){
        size_t cap = w->cap ? w->cap * 2 : 1024;
        while(cap < w->len + (size_t)len + 1)
            cap *= 2;
        char* b = realloc(w->buf, cap);
        if(!b){
            w->err = CAPE_E_CAP;
            return;
        }
        w->buf = b;
        w->cap = cap;
    }
    if(w->n == w->ncap){
        const size_t ncap = w->ncap ? w->ncap * 2 : 64;
        size_t* o = realloc(w->off, ncap * sizeof(*o));
        if(!o){
            w->err = CAPE_E_CAP;
            return;
        }
        w->off = o;
        w->ncap = ncap;
    }
    w->off[w->n++] = w->len;
    memcpy(w->buf + w->len, tmp, (size_t)len + 1);
    w->len += (size_t)len + 1;
}

// The length of the UTF-8 character that starts at s[i] (text that is valid UTF-8)
static size_t char_len(const uint8_t* s, size_t n, size_t i){
    size_t k = 1;
    while(i + k < n && (s[i + k] & 0xc0) == 0x80)
        k++;
    return k;
}

static size_t enc_len(const uint8_t* s, size_t n){
    size_t e = 0;
    for(size_t i = 0; i < n; i++)
        e += ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.'
              || s[i] == '_' || s[i] == '~') ? 1 : 3;
    return e;
}

// Cuts UTF-8 text into pieces of at most CAPE_HWSLOT_TEXT_MAX encoded bytes, each with as many whole characters as fit. Returns
// the number of pieces (at most max) and their ends in end[], or 0 if more are needed.
static unsigned cut_text(const uint8_t* s, size_t n, size_t* end, unsigned max){
    unsigned pieces = 0;
    size_t i = 0;
    if(n == 0){
        end[0] = 0;
        return 1;
    }
    while(i < n){
        if(pieces == max)
            return 0;
        size_t e = 0;
        size_t j = i;
        while(j < n){
            const size_t k = char_len(s, n, j);
            const size_t ek = enc_len(s + j, k);
            if(e + ek > CAPE_HWSLOT_TEXT_MAX)
                break;
            e += ek;
            j += k;
        }
        end[pieces++] = j;
        i = j;
    }
    return pieces;
}

static void add_text(words* w, const char* prefix, const uint8_t* s, size_t n){
    size_t end[CAPE_HWSLOT_NAME_WORDS];
    const unsigned pieces = cut_text(s, n, end, CAPE_HWSLOT_NAME_WORDS);
    if(!pieces){
        if(!w->err)
            w->err = CAPE_E_RANGE;
        return;
    }
    size_t start = 0;
    for(unsigned i = 0; i < pieces; i++){
        char enc[CAPE_HWSLOT_TEXT_MAX + 1];
        if(cape_hwslot_encode(s + start, end[i] - start, enc, sizeof(enc)) < 0){
            w->err = CAPE_E_RANGE;
            return;
        }
        add(w, "%s:%u/%u:%s", prefix, i + 1, pieces, enc);
        start = end[i];
    }
}

// A reason is shown, not kept: one word, cut short at a character
static void add_reason(words* w, const char* prefix, const char* why){
    const uint8_t* s = (const uint8_t*)why;
    size_t n = strlen(why), end[1];
    if(!cut_text(s, n, end, 1)){
        // longer than a word: as many characters as fit
        size_t e = 0, j = 0;
        while(j < n){
            const size_t k = char_len(s, n, j);
            const size_t ek = enc_len(s + j, k);
            if(e + ek > CAPE_HWSLOT_TEXT_MAX)
                break;
            e += ek;
            j += k;
        }
        n = j;
    }
    char enc[CAPE_HWSLOT_TEXT_MAX + 1];
    if(cape_hwslot_encode(s, n, enc, sizeof(enc)) < 0){
        w->err = CAPE_E_RANGE;
        return;
    }
    add(w, "%s:%s", prefix, enc);
}

static const char* light_name(cape_hwbind_light l){
    switch(l){
    case CAPE_HWBIND_LIGHT_STATIC:
        return "static";
    case CAPE_HWBIND_LIGHT_EMPTY:
        return "empty";
    case CAPE_HWBIND_LIGHT_EFFECTS:
        return "effects";
    case CAPE_HWBIND_LIGHT_UNKNOWN:
        break;
    }
    return "unknown";
}

static void add_events(words* w, const char* prefix, unsigned key, const cape_event* ev, unsigned n){
    // As many whole events as fit in CAPE_HWSLOT_EVENT_BYTES, as in the file (cape_hwslot.h)
    for(unsigned off = 0; off < n;){
        char hex[2 * CAPE_HWSLOT_EVENT_BYTES + 1];
        size_t at = 0;
        unsigned count = 0;
        while(off + count < n){
            const cape_event e = ev[off + count];
            const unsigned size = cape_ev_size(e);
            if(at / 2 + size > CAPE_HWSLOT_EVENT_BYTES)
                break;
            if(size == 4)
                snprintf(hex + at, 9, "%02x%02x%02x%02x", e.b0, e.b1, e.b2, e.b3);
            else
                snprintf(hex + at, 5, "%02x%02x", e.b0, e.b1);
            at += 2 * size;
            count++;
        }
        hex[at] = '\0';
        add(w, "%s:%02x:%u:%s", prefix, key, off, hex);
        off += count;
    }
}

static void make_words(const cape_hwbind_slot* s, words* w){
    const cape_binding_model* b = s->bind;
    add(w, "v:1");
    if(b->state == CAPE_BIND_EMPTY){
        add(w, "id:0");
        add(w, "st:empty");
        add(w, "lt:empty");
        add(w, "ly:0");
        add(w, "end");
        return;
    }
    char id[CAPE_HWSLOT_ID_MAX];
    cape_hwslot_id(s->entry, id);
    add(w, "id:%s", id);
    if(b->state == CAPE_BIND_OK)
        add(w, "st:ok");
    else
        add_reason(w, b->state == CAPE_BIND_RAW ? "st:raw" : "st:broken", b->reason);
    if(s->readonly)
        add_reason(w, "ro", s->readonly);
    if(s->name){
        uint8_t utf8[CAPE_INFO_NAME_UNITS * 3 + 1];
        const int n = cape_hwslot_utf8(s->name, s->name_units, utf8, sizeof(utf8));
        if(n < 0){
            w->err = CAPE_E_RANGE;
            return;
        }
        add_text(w, "nm", utf8, (size_t)n);
    }
    if(b->winlock_read)
        add(w, "wl:%02x", b->winlock);
    if(s->indicators){
        char hex[25];
        for(int i = 0; i < 12; i++)
            snprintf(hex + 2 * i, 3, "%02x", s->indicators[i]);
        add(w, "ind:%s", hex);
    }
    add(w, "lt:%s", light_name(s->light));
    add(w, "ly:%u", s->layers);
    if(b->state == CAPE_BIND_OK){
        for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++)
            if(b->key[k].kind == CAPE_BIND_REMAP)
                add(w, "m:%02x:%02x", k, b->key[k].dst);
        for(unsigned k = 0; k < CAPE_BIND_K95_KEYS; k++){
            const cape_binding_action* a = &b->key[k];
            if(a->kind != CAPE_BIND_MACRO)
                continue;
            add(w, "a:%02x:%s:%02x:%02x:%02x:%02x:%u:%02x", k, a->file[0] ? a->file : "-", a->subtype, a->start, a->run, a->repeat,
                a->nevents, a->flags);
            add_events(w, "e", k, a->events, a->nevents);
        }
    }
    add(w, "end");
}

static size_t prefix_len(unsigned m, unsigned gen, unsigned p, unsigned pages){
    char tmp[64];
    return (size_t)snprintf(tmp, sizeof(tmp), "mode %u hwbind %u %u/%u ", m, gen, p, pages);
}

// Pages for a given count of pages (the prefix of each line depends on it): returns how many pages the words take that way
static unsigned paginate(const cape_hwbind_record* r, const size_t* len, unsigned pages, size_t* first){
    unsigned p = 0;
    size_t i = 0;
    while(i < r->nwords){
        first[p] = i;
        size_t used = prefix_len(r->m, r->gen, p + 1, pages) + len[i] + 1;   // the first word and the newline
        i++;
        while(i < r->nwords && used + 1 + len[i] <= CAPE_HWSLOT_LINE_MAX){
            used += 1 + len[i];
            i++;
        }
        p++;
    }
    first[p] = r->nwords;
    return p;
}

void cape_hwbind_free(cape_hwbind_record* r){
    if(!r)
        return;
    free(r->words);
    free(r->word);
    free(r->first);
    memset(r, 0, sizeof(*r));
}

int cape_hwbind_make(const cape_hwbind_slot* s, unsigned m, unsigned gen, cape_hwbind_record* out){
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!s || !s->bind || m < 1 || m > 3 || (s->name == NULL && s->name_units))
        return CAPE_E_ARG;
    words w = {0};
    make_words(s, &w);
    if(w.err){
        free(w.buf);
        free(w.off);
        return w.err;
    }
    out->m = m;
    out->gen = gen;
    out->words = w.buf;
    out->word = w.off;
    out->nwords = w.n;
    size_t* len = malloc(w.n * sizeof(*len));
    out->first = malloc((w.n + 1) * sizeof(*out->first));
    if(!len || !out->first){
        free(len);
        cape_hwbind_free(out);
        return CAPE_E_CAP;
    }
    for(size_t i = 0; i < w.n; i++)
        len[i] = strlen(w.buf + w.off[i]);
    // The count of pages is in every prefix: settle it (a longer count can only need more pages, so this ends)
    unsigned pages = 1;
    for(int round = 0; round < 16; round++){
        const unsigned again = paginate(out, len, pages, out->first);
        if(again == pages)
            break;
        pages = again;
    }
    out->pages = paginate(out, len, pages, out->first);
    free(len);
    if(out->pages != pages){
        cape_hwbind_free(out);
        return CAPE_E_RANGE;
    }
    return CAPE_OK;
}

int cape_hwbind_line(const cape_hwbind_record* r, unsigned p, char* out, size_t cap){
    if(!r || !out)
        return CAPE_E_ARG;
    if(p < 1 || p > r->pages)
        return CAPE_E_RANGE;
    int n = snprintf(out, cap, "mode %u hwbind %u %u/%u", r->m, r->gen, p, r->pages);
    if(n < 0 || (size_t)n >= cap)
        return CAPE_E_CAP;
    size_t len = (size_t)n;
    for(size_t i = r->first[p - 1]; i < r->first[p]; i++){
        const char* word = r->words + r->word[i];
        const size_t wl = strlen(word);
        if(len + 1 + wl + 1 >= cap)
            return CAPE_E_CAP;
        out[len++] = ' ';
        memcpy(out + len, word, wl);
        len += wl;
    }
    if(len + 1 >= cap)
        return CAPE_E_CAP;
    out[len++] = '\n';
    out[len] = '\0';
    return (int)len;
}

int cape_hwbind_error_line(unsigned m, unsigned gen, const char* why, char* out, size_t cap){
    if(!why || !out)
        return CAPE_E_ARG;
    const int n = snprintf(out, cap, "mode %u hwbind %u error %s\n", m, gen, why);
    if(n < 0 || (size_t)n >= cap)
        return CAPE_E_CAP;
    return n;
}
