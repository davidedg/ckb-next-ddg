#include "cape_lightmodel.h"
#include "cape_hwslot.h"

#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One collection also has its count child in XML. The aggregate bounds are deliberately weaker than the XML writer's:
// full-profile validation still applies at preflight, while the receiver has bounded memory before preflight.
#define COLLECTION_MAX (CAPE_XML_CHILDREN_MAX - 1u)
enum { START, LAYER, LNAME, SELECT, KEY, RUN, EFFECT, ENAME, ANIMATION, CURVE, POINT, PARAM, LEND, DONE };
enum { U32, REAL, BOOL, SPREAD, COLOURS };
typedef struct { const char* name; size_t offset; int type; } parameter;
#define P(field, type) { #field, offsetof(cape_profile_effect, field), type }
static const parameter static_params[] = { P(opacity, U32) };
static const parameter wave_params[] = { P(velocity, REAL), P(tail_length, REAL), P(double_sided, BOOL), P(angle, REAL) };
static const parameter ripple_params[] = { P(velocity, REAL), P(tail_length, REAL), P(auto_duration, BOOL), P(wave_spread, SPREAD),
                                         P(colours, COLOURS) };
static const parameter pulse_params[] = { P(colours, COLOURS), P(random, BOOL), P(speed, U32) };
static const parameter colour_wave_params[] = { P(colours, COLOURS), P(random, BOOL), P(speed, U32), P(direction, U32) };
static const parameter rain_params[] = { P(colours, COLOURS), P(random, BOOL), P(opacity, U32), P(speed, U32), P(direction, U32) };
static const parameter rainbow_params[] = { P(speed, U32), P(direction, U32) };
static const parameter spiral_params[] = { P(clockwise, BOOL), P(speed, U32) };
static const parameter type_params[] = { P(colours, COLOURS), P(random, BOOL), P(opacity, U32), P(speed, U32), P(type_duration, U32),
                                       P(type_behaviour, U32) };
#undef P
typedef struct { const char* name; const parameter* params; unsigned nparams; int curve; } effect_type;
#define LIST(name, params, curve) { name, params, sizeof(params) / sizeof(params[0]), curve }
static const effect_type types[] = {
    LIST("static", static_params, 1), { "solid", NULL, 0, 1 }, { "gradient", NULL, 0, 1 }, LIST("wave", wave_params, 1),
    LIST("ripple", ripple_params, 1), LIST("color-pulse", pulse_params, 0), LIST("color-shift", pulse_params, 0),
    LIST("color-wave", colour_wave_params, 0), LIST("rain", rain_params, 0), LIST("rainbow-wave", rainbow_params, 0),
    LIST("spiral-rainbow", spiral_params, 0), LIST("type", type_params, 0), LIST("visor", colour_wave_params, 0)
};
_Static_assert(sizeof(types) / sizeof(types[0]) == CAPE_PROFILE_EFFECTS, "every lighting type needs a wire schema");
#undef LIST
static const char* const plays[] = { "standard", "from-key-center", "on-key-center" };
static const char* const spreads[] = { "between-fronts", "after-inner", "before-outer", "every" };

struct cape_lightmodel_reader {
    cape_lightmodel model;
    int phase, error;
    uint32_t layer, item, parameter, active, seen_active;
    unsigned part, parts;
    size_t text_bytes, elements;
};

void cape_lightmodel_free(cape_lightmodel* model){
    if(!model)
        return;
    for(uint32_t i = 0; i < model->count; ++i){
        cape_profile_layer* l = &model->layers[i];
        free(l->name.p);
        for(uint32_t j = 0; j < l->nkeys; ++j)
            free(l->keys[j].p);
        free(l->keys);
        free(l->effect.name.p);
        free(l->effect.transitions);
    }
    free(model->layers);
    memset(model, 0, sizeof(*model));
}

static int text_copy(const cape_profile_text* s, cape_profile_text* d){
    d->p = NULL;
    d->len = s->len;
    if(!s->len)
        return CAPE_OK;
    d->p = malloc((size_t)s->len + 1);
    if(!d->p)
        return CAPE_E_CAP;
    memcpy(d->p, s->p, s->len);
    d->p[s->len] = 0;
    return CAPE_OK;
}

int cape_lightmodel_copy(const cape_lightmodel* source, cape_lightmodel* out){
    if(!source || !out || source == out || source->count > COLLECTION_MAX || (!source->layers && source->count) ||
       (!source->known && source->count) || (source->known != 0 && source->known != 1))
        return CAPE_E_ARG;
    for(uint32_t i = 0; i < source->count; ++i){
        const cape_profile_layer* l = &source->layers[i];
        if(l->nkeys > COLLECTION_MAX || (!l->keys && l->nkeys) || l->effect.ntransitions > COLLECTION_MAX ||
           (!l->effect.transitions && l->effect.ntransitions) || l->name.len > CAPE_XML_VALUE_MAX ||
           (!l->name.p && l->name.len) || l->effect.name.len > CAPE_XML_VALUE_MAX || (!l->effect.name.p && l->effect.name.len))
            return CAPE_E_RANGE;
        if(!cape_profile_solid_valid(&l->effect)) return CAPE_E_RANGE;
        for(uint32_t j = 0; j < l->nkeys; ++j)
            if(l->keys[j].len > CAPE_XML_VALUE_MAX || (!l->keys[j].p && l->keys[j].len)) return CAPE_E_RANGE;
    }
    cape_lightmodel m = { .known = source->known, .count = source->count };
    if(m.count){
        m.layers = calloc(m.count, sizeof(*m.layers));
        if(!m.layers)
            return CAPE_E_CAP;
    }
    int rc = CAPE_OK;
    for(uint32_t i = 0; i < m.count && rc == CAPE_OK; ++i){
        const cape_profile_layer* s = &source->layers[i];
        cape_profile_layer* d = &m.layers[i];
        *d = *s;
        d->name.p = d->effect.name.p = NULL;
        d->keys = NULL;
        d->nkeys = 0;
        d->effect.transitions = NULL;
        rc = text_copy(&s->name, &d->name);
        if(rc == CAPE_OK)
            rc = text_copy(&s->effect.name, &d->effect.name);
        if(rc == CAPE_OK && s->nkeys){
            d->keys = calloc(s->nkeys, sizeof(*d->keys));
            if(!d->keys)
                rc = CAPE_E_CAP;
            else {
                d->nkeys = s->nkeys;
                for(uint32_t j = 0; j < s->nkeys && rc == CAPE_OK; ++j)
                    rc = text_copy(&s->keys[j], &d->keys[j]);
            }
        }
        if(rc == CAPE_OK && s->effect.ntransitions){
            const size_t bytes = (size_t)s->effect.ntransitions * sizeof(*d->effect.transitions);
            d->effect.transitions = malloc(bytes);
            if(!d->effect.transitions)
                rc = CAPE_E_CAP;
            else
                memcpy(d->effect.transitions, s->effect.transitions, bytes);
        }
    }
    if(rc != CAPE_OK)
        cape_lightmodel_free(&m);
    else
        *out = m;
    return rc;
}

static int number(const char* s, uint32_t max, uint32_t* out){
    if(!*s || (s[0] == '0' && s[1]))
        return 0;
    uint32_t n = 0;
    for(; *s; ++s){
        if(*s < '0' || *s > '9' || n > max / 10 || (n == max / 10 && (unsigned)(*s - '0') > max % 10))
            return 0;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return 1;
}

static int signed_number(const char* s, int32_t* out){
    const int negative = *s == '-';
    uint32_t n;
    if(!number(s + negative, negative ? (uint32_t)INT32_MAX + 1u : INT32_MAX, &n) || (negative && !n))
        return 0;
    *out = negative ? (n == (uint32_t)INT32_MAX + 1u ? INT32_MIN : -(int32_t)n) : (int32_t)n;
    return 1;
}

static int hex(const char* s, size_t n, uint32_t* out){
    if(strlen(s) != n)
        return 0;
    *out = 0;
    for(size_t i = 0; i < n; ++i){
        const char c = s[i];
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
        *out = (*out << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return 1;
}

static int uuid(const char* s, cape_profile_uuid* out){
    if(strlen(s) != 32)
        return 0;
    int nonzero = 0;
    for(unsigned i = 0; i < 16; ++i){
        char pair[3] = { s[2 * i], s[2 * i + 1], 0 };
        uint32_t v;
        if(!hex(pair, 2, &v))
            return 0;
        out->b[i] = (uint8_t)v;
        nonzero |= v;
    }
    return nonzero != 0;
}

static const char* decimal_point(void){
    const struct lconv* lc = localeconv();
    return lc && lc->decimal_point && *lc->decimal_point ? lc->decimal_point : ".";
}

static int real(const char* s, double* out){
    size_t i = s[0] == '-' ? 1 : 0, start = i;
    while(s[i] >= '0' && s[i] <= '9') ++i;
    if(i == start) return 0;
    if(s[i] == '.'){
        start = ++i;
        while(s[i] >= '0' && s[i] <= '9') ++i;
        if(i == start) return 0;
    }
    if(s[i] == 'e' || s[i] == 'E'){
        ++i;
        if(s[i] == '+' || s[i] == '-') ++i;
        start = i;
        while(s[i] >= '0' && s[i] <= '9') ++i;
        if(i == start) return 0;
    }
    if(s[i] || i > 40) return 0;
    char local[80];
    const char* point = decimal_point();
    const size_t plen = strlen(point);
    size_t j = 0;
    for(size_t k = 0; k < i; ++k){
        const size_t need = s[k] == '.' ? plen : 1;
        if(need >= sizeof(local) - j) return 0;
        if(s[k] == '.') memcpy(local + j, point, plen);
        else local[j] = s[k];
        j += need;
    }
    local[j] = 0;
    char* end;
    *out = strtod(local, &end);
    return !*end && isfinite(*out);
}

static int real_text(double value, char out[80]){
    char local[80];
    if(!isfinite(value)) return 0;
    const int n = snprintf(local, sizeof(local), "%.17g", value);
    if(n < 0 || (size_t)n >= sizeof(local)) return 0;
    const char* point = decimal_point();
    const size_t plen = strlen(point);
    const char* p = strstr(local, point);
    if(p && strcmp(point, ".")){
        const size_t prefix = (size_t)(p - local);
        memcpy(out, local, prefix);
        out[prefix] = '.';
        strcpy(out + prefix + 1, p + plen);
    } else
        strcpy(out, local);
    return 1;
}

static int named(const char* s, const char* const* names, unsigned count){
    for(unsigned i = 0; i < count; ++i)
        if(!strcmp(s, names[i])) return (int)i;
    return -1;
}

static int valid_text(const uint8_t* bytes, size_t n){
    // This also rejects NUL, overlong encodings and surrogate code points.
    for(size_t i = 0; i < n; ++i){
        if(bytes[i] < 0x20 && bytes[i] != '\t' && bytes[i] != '\n' && bytes[i] != '\r') return 0;
        if(i + 2 < n && bytes[i] == 0xef && bytes[i + 1] == 0xbf && bytes[i + 2] >= 0xbe) return 0;
    }
    // Fragments are small; larger source strings are validated a character at a time by the emitter.
    uint16_t units[CAPE_HWSLOT_TEXT_MAX];
    return cape_hwslot_utf16(bytes, n, units, sizeof(units) / sizeof(units[0])) >= 0;
}

static int identities(const cape_lightmodel* m){
    // At most twice the XML collection bound. Keep the check independent from profile/action identities, checked at preflight.
    for(uint32_t i = 0; i < m->count; ++i){
        const cape_profile_uuid* ids[] = { &m->layers[i].id, &m->layers[i].effect.id };
        for(unsigned k = 0; k < 2; ++k){
            uint8_t any = 0;
            for(unsigned b = 0; b < 16; ++b) any |= ids[k]->b[b];
            if(!any) return 0;
            if(k && !memcmp(ids[0], ids[1], sizeof(*ids[0]))) return 0;
            for(uint32_t j = 0; j < i; ++j)
                if(!memcmp(ids[k], &m->layers[j].id, sizeof(*ids[k])) ||
                   !memcmp(ids[k], &m->layers[j].effect.id, sizeof(*ids[k]))) return 0;
        }
    }
    return 1;
}

cape_lightmodel_reader* cape_lightmodel_reader_new(void){
    return calloc(1, sizeof(cape_lightmodel_reader));
}

void cape_lightmodel_reader_free(cape_lightmodel_reader* r){
    if(r){ cape_lightmodel_free(&r->model); free(r); }
}

static int text_part(cape_lightmodel_reader* r, cape_profile_text* text, const char* part, const char* encoded){
    const char* slash = strchr(part, '/');
    if(!slash || slash - part > 10) return 0;
    char first[12];
    memcpy(first, part, (size_t)(slash - part));
    first[slash - part] = 0;
    uint32_t p, count;
    // Even one decoded byte per fragment cannot require more than VALUE_MAX fragments; empty text has one.
    if(!number(first, CAPE_XML_VALUE_MAX, &p) || !number(slash + 1, CAPE_XML_VALUE_MAX, &count) || !p || !count || p > count ||
       p != r->part + 1 || (r->part && count != r->parts) || strlen(encoded) > CAPE_HWSLOT_TEXT_MAX) return 0;
    uint8_t bytes[CAPE_HWSLOT_TEXT_MAX];
    const int n = cape_hwslot_decode(encoded, strlen(encoded), bytes, sizeof(bytes));
    if(n < 0 || !valid_text(bytes, (size_t)n) || (!n && (count != 1 || p != 1)) ||
       (size_t)n > CAPE_XML_VALUE_MAX - text->len || (size_t)n > CAPE_XML_DOC_MAX - r->text_bytes) return 0;
    char* data = realloc(text->p, (size_t)text->len + (size_t)n + 1);
    if(!data){ r->error = CAPE_E_CAP; return 0; }
    text->p = data;
    memcpy(data + text->len, bytes, (size_t)n);
    text->len += (uint32_t)n;
    data[text->len] = 0;
    r->text_bytes += (size_t)n;
    r->part = p;
    r->parts = count;
    if(p == count) r->part = r->parts = 0;
    return 1;
}

static int array(cape_lightmodel_reader* r, const char* count, size_t size, void** out, uint32_t* n){
    if(!number(count, COLLECTION_MAX, n) || *n > CAPE_XML_NODES_MAX - r->elements) return 0;
    *out = *n ? calloc(*n, size) : NULL;
    if(*n && !*out){ r->error = CAPE_E_CAP; *n = 0; return 0; }
    r->elements += *n;
    return 1;
}

static int read_parameter(cape_profile_effect* e, const parameter* p, char** f, unsigned n){
    void* dst = (char*)e + p->offset;
    uint32_t v;
    double d;
    if(strcmp(f[2], p->name) || n != (p->type == COLOURS ? 5u : 4u)) return 0;
    switch(p->type){
    case U32: if(!number(f[3], UINT32_MAX, &v)) return 0; memcpy(dst, &v, sizeof(v)); break;
    case REAL: if(!real(f[3], &d)) return 0; memcpy(dst, &d, sizeof(d)); break;
    case BOOL: if(!number(f[3], 1, &v)) return 0; *(uint8_t*)dst = (uint8_t)v; break;
    case SPREAD: {
        const int k = named(f[3], spreads, 4);
        if(k < 0) return 0;
        *(uint8_t*)dst = (uint8_t)k;
        break;
    }
    case COLOURS: return hex(f[3], 8, &e->colours[0]) && hex(f[4], 8, &e->colours[1]);
    default: return 0;
    }
    return 1;
}

static int consume(cape_lightmodel_reader* r, char** f, unsigned n){
    uint32_t v;
    void* memory = NULL;
    if(r->phase == START){
        if(n == 2 && !strcmp(f[0], "lights") && !strcmp(f[1], "unknown")){ r->phase = DONE; return 1; }
        if(n != 4 || strcmp(f[0], "lights") || strcmp(f[1], "known") ||
           !number(f[3], COLLECTION_MAX, &r->active) ||
           !array(r, f[2], sizeof(*r->model.layers), &memory, &r->model.count)) return 0;
        r->model.layers = memory;
        if(r->active > r->model.count) return 0;
        r->model.known = 1;
        r->phase = r->model.count ? LAYER : DONE;
        return 1;
    }
    if(r->phase == DONE || n < 2 || !number(f[1], COLLECTION_MAX, &v) || v != r->layer) return 0;
    cape_profile_layer* l = &r->model.layers[r->layer];
    cape_profile_effect* e = &l->effect;
    switch(r->phase){
    case LAYER:
        if(n != 4 || strcmp(f[0], "layer") || !uuid(f[2], &l->id) || !number(f[3], 1, &v)) return 0;
        l->enabled = (uint8_t)v; r->seen_active += v; r->phase = LNAME; return 1;
    case LNAME:
    case ENAME:
        if(n != 4 || strcmp(f[0], r->phase == LNAME ? "lname" : "ename") ||
           !text_part(r, r->phase == LNAME ? &l->name : &e->name, f[2], f[3])) return 0;
        if(!r->part) r->phase = r->phase == LNAME ? SELECT : ANIMATION;
        return 1;
    case SELECT:
        if(n != 4 || strcmp(f[0], "select")) return 0;
        if(!strcmp(f[2], "all")) l->option = CAPE_PROFILE_OPTION_ALL;
        else if(!strcmp(f[2], "concrete")) l->option = CAPE_PROFILE_OPTION_CONCRETE;
        else return 0;
        if(!array(r, f[3], sizeof(*l->keys), &memory, &l->nkeys)) return 0;
        l->keys = memory;
        r->item = 0; r->phase = l->nkeys ? KEY : RUN; return 1;
    case KEY:
        if(n != 5 || strcmp(f[0], "key") || !number(f[2], COLLECTION_MAX, &v) || v != r->item ||
           !text_part(r, &l->keys[r->item], f[3], f[4])) return 0;
        if(!r->part && ++r->item == l->nkeys) r->phase = RUN;
        return 1;
    case RUN: {
        if(n != 8 || strcmp(f[0], "run")) return 0;
        uint8_t* flags[] = { &l->start_on_key_press, &l->start_with_profile, &l->stop_on_key_press, &l->stop_on_key_release };
        for(unsigned i = 0; i < 4; ++i){ if(!number(f[2 + i], 1, &v)) return 0; *flags[i] = (uint8_t)v; }
        const int p = named(f[7], plays, 3);
        if(p < 0 || !signed_number(f[6], &l->stop_after_times)) return 0;
        l->play_option = (uint8_t)p; r->phase = EFFECT; return 1;
    }
    case EFFECT:
        if(n != 4 || strcmp(f[0], "effect") || !uuid(f[3], &e->id)) return 0;
        for(unsigned i = 0; i < CAPE_PROFILE_EFFECTS; ++i)
            if(!strcmp(f[2], types[i].name)){ e->kind = (cape_profile_effect_kind)i; r->phase = ENAME; return 1; }
        return 0;
    case ANIMATION:
        if(n != 4 || strcmp(f[0], "animation") || !number(f[2], UINT32_MAX, &e->duration) || !real(f[3], &e->brightness)) return 0;
        r->parameter = 0;
        r->phase = types[e->kind].curve ? CURVE : (types[e->kind].nparams ? PARAM : LEND);
        return 1;
    case CURVE:
        if(n != 3 || strcmp(f[0], "curve") ||
           !array(r, f[2], sizeof(*e->transitions), &memory, &e->ntransitions)) return 0;
        e->transitions = memory;
        r->item = 0; r->phase = e->ntransitions ? POINT : (types[e->kind].nparams ? PARAM : LEND); return 1;
    case POINT: {
        if((n != 8 && n != 9) || strcmp(f[0], "point") || !number(f[2], COLLECTION_MAX, &v) || v != r->item) return 0;
        cape_profile_transition* t = &e->transitions[r->item];
        if(!real(f[3], &t->time)) return 0;
        if(n == 8 && !strcmp(f[4], "rgb")) t->colour.has_alpha = 0;
        else if(n == 9 && !strcmp(f[4], "rgba")) t->colour.has_alpha = 1;
        else return 0;
        for(unsigned i = 0; i < n - 5; ++i)
            if(!real(f[5 + i], &t->colour.c[i]) || t->colour.c[i] < 0 || t->colour.c[i] > 1) return 0;
        if(++r->item == e->ntransitions) r->phase = types[e->kind].nparams ? PARAM : LEND;
        return 1;
    }
    case PARAM:
        if(n < 4 || strcmp(f[0], "param") || !read_parameter(e, &types[e->kind].params[r->parameter], f, n)) return 0;
        if(++r->parameter == types[e->kind].nparams) r->phase = LEND;
        return 1;
    case LEND:
        if(n != 2 || strcmp(f[0], "lend") || cape_profile_layer_error(l)) return 0;
        if(++r->layer == r->model.count){
            if(r->active != r->seen_active || !identities(&r->model)) return 0;
            r->phase = DONE;
        } else r->phase = LAYER;
        return 1;
    default: return 0;
    }
}

int cape_lightmodel_read_word(cape_lightmodel_reader* r, const char* word){
    if(!r || !word) return CAPE_E_ARG;
    if(r->error) return r->error;
    const size_t len = strlen(word);
    if(!len || len > CAPE_HWSLOT_WORD_MAX) return r->error = CAPE_E_RANGE;
    char buffer[CAPE_HWSLOT_WORD_MAX + 1];
    memcpy(buffer, word, len + 1);
    char* f[12] = { buffer };
    unsigned n = 1;
    for(size_t i = 0; i < len; ++i)
        if(buffer[i] == ':'){
            if(n == sizeof(f) / sizeof(f[0])) return r->error = CAPE_E_RANGE;
            buffer[i] = 0; f[n++] = buffer + i + 1;
        }
    if(!consume(r, f, n)) return r->error = r->error ? r->error : CAPE_E_RANGE;
    return CAPE_OK;
}

int cape_lightmodel_ready(const cape_lightmodel_reader* r){
    return r && !r->error && r->phase == DONE;
}

int cape_lightmodel_take(cape_lightmodel_reader* r, cape_lightmodel* out){
    if(!out || !cape_lightmodel_ready(r)) return CAPE_E_ARG;
    *out = r->model;
    memset(&r->model, 0, sizeof(r->model));
    r->error = CAPE_E_ARG; // moved; a reader cannot publish twice
    return CAPE_OK;
}

typedef struct { cape_lightmodel_word emit; void* context; const char* txn; } writer;

static int word(writer* w, const char* text){
    if(strlen(text) > CAPE_HWSLOT_WORD_MAX) return CAPE_E_RANGE;
    if(!w->txn) return w->emit(w->context, text);
    const char* colon = strchr(text, ':');
    if(!colon) return CAPE_E_ARG;
    char staged[CAPE_HWSLOT_WORD_MAX + 1];
    const int n = snprintf(staged, sizeof(staged), "%.*s:%s%s", (int)(colon - text), text, w->txn, colon);
    if(n < 0 || (size_t)n >= sizeof(staged)) return CAPE_E_RANGE;
    return w->emit(w->context, staged);
}

static void uuid_text(const cape_profile_uuid* id, char out[33]){
    static const char digits[] = "0123456789abcdef";
    for(unsigned i = 0; i < 16; ++i){ out[2 * i] = digits[id->b[i] >> 4]; out[2 * i + 1] = digits[id->b[i] & 15]; }
    out[32] = 0;
}

// The encoded size of one UTF-8 character. Validation occurs before it is emitted.
static size_t character(const char* p, size_t left, size_t* bytes){
    const unsigned char c = (unsigned char)*p;
    const size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if(n > left || !valid_text((const uint8_t*)p, n)) return 0;
    char encoded[13];
    const int len = cape_hwslot_encode((const uint8_t*)p, n, encoded, sizeof(encoded));
    *bytes = n;
    return len > 0 ? (size_t)len : 0;
}

static int text(writer* w, const char* prefix, const cape_profile_text* t){
    if(t->len > CAPE_XML_VALUE_MAX || (!t->p && t->len)) return CAPE_E_RANGE;
    // At most one fragment per decoded byte; no fixed name-fragment limit.
    size_t start = 0, end = 0;
    unsigned parts = 1;
    size_t encoded = 0;
    while(end < t->len){
        size_t n;
        const size_t need = character(t->p + end, t->len - end, &n);
        if(!need) return CAPE_E_RANGE;
        if(encoded + need > CAPE_HWSLOT_TEXT_MAX){ ++parts; encoded = 0; }
        encoded += need; end += n;
    }
    for(unsigned part = 1; part <= parts; ++part){
        end = start; encoded = 0;
        while(end < t->len){
            size_t n;
            const size_t need = character(t->p + end, t->len - end, &n);
            if(!need) return CAPE_E_RANGE;
            if(encoded + need > CAPE_HWSLOT_TEXT_MAX) break;
            encoded += need; end += n;
        }
        char escaped[CAPE_HWSLOT_TEXT_MAX + 1], output[CAPE_HWSLOT_WORD_MAX + 1];
        const uint8_t* bytes = t->p ? (const uint8_t*)t->p + start : NULL;
        if(cape_hwslot_encode(bytes, end - start, escaped, sizeof(escaped)) < 0) return CAPE_E_RANGE;
        const int len = snprintf(output, sizeof(output), "%s:%u/%u:%s", prefix, part, parts, escaped);
        if(len < 0 || (size_t)len >= sizeof(output)) return CAPE_E_RANGE;
        const int rc = word(w, output);
        if(rc != CAPE_OK) return rc;
        start = end;
    }
    return CAPE_OK;
}

#define TRY(call) do { int result_ = (call); if(result_ != CAPE_OK) return result_; } while(0)
#define SAY(...) do { int length_ = snprintf(buffer, sizeof(buffer), __VA_ARGS__); \
    if(length_ < 0 || (size_t)length_ >= sizeof(buffer)){ return CAPE_E_RANGE; } TRY(word(w, buffer)); } while(0)

static int emit_model(const cape_lightmodel* m, writer* w){
    char buffer[CAPE_HWSLOT_WORD_MAX + 1], prefix[64], id[33], a[80], b[80], c[80], d[80], time[80];
    if(!m->known){ SAY("lights:unknown"); return CAPE_OK; }
    unsigned active = 0;
    for(uint32_t i = 0; i < m->count; ++i) active += m->layers[i].enabled;
    SAY("lights:known:%u:%u", m->count, active);
    for(uint32_t i = 0; i < m->count; ++i){
        const cape_profile_layer* l = &m->layers[i];
        const cape_profile_effect* e = &l->effect;
        if(e->kind >= CAPE_PROFILE_EFFECTS || l->play_option >= 3 || l->option > 1) return CAPE_E_RANGE;
        uuid_text(&l->id, id); SAY("layer:%u:%s:%u", i, id, l->enabled);
        snprintf(prefix, sizeof(prefix), "lname:%u", i); TRY(text(w, prefix, &l->name));
        SAY("select:%u:%s:%u", i, l->option == CAPE_PROFILE_OPTION_ALL ? "all" : "concrete", l->nkeys);
        for(uint32_t j = 0; j < l->nkeys; ++j){
            snprintf(prefix, sizeof(prefix), "key:%u:%u", i, j); TRY(text(w, prefix, &l->keys[j]));
        }
        SAY("run:%u:%u:%u:%u:%u:%d:%s", i, l->start_on_key_press, l->start_with_profile, l->stop_on_key_press,
            l->stop_on_key_release, (int)l->stop_after_times, plays[l->play_option]);
        uuid_text(&e->id, id); SAY("effect:%u:%s:%s", i, types[e->kind].name, id);
        snprintf(prefix, sizeof(prefix), "ename:%u", i); TRY(text(w, prefix, &e->name));
        if(!real_text(e->brightness, a)) return CAPE_E_RANGE;
        SAY("animation:%u:%u:%s", i, e->duration, a);
        if(types[e->kind].curve){
            SAY("curve:%u:%u", i, e->ntransitions);
            for(uint32_t j = 0; j < e->ntransitions; ++j){
                const cape_profile_transition* t = &e->transitions[j];
                if(t->colour.has_alpha > 1 || !real_text(t->time, time) || !real_text(t->colour.c[0], a) || !real_text(t->colour.c[1], b) ||
                   !real_text(t->colour.c[2], c)) return CAPE_E_RANGE;
                if(t->colour.has_alpha){
                    if(!real_text(t->colour.c[3], d)) return CAPE_E_RANGE;
                    SAY("point:%u:%u:%s:rgba:%s:%s:%s:%s", i, j, time, a, b, c, d);
                } else { SAY("point:%u:%u:%s:rgb:%s:%s:%s", i, j, time, a, b, c); }
            }
        }
        for(unsigned j = 0; j < types[e->kind].nparams; ++j){
            const parameter* p = &types[e->kind].params[j];
            const void* src = (const char*)e + p->offset;
            uint32_t value;
            double value_real;
            switch(p->type){
            case U32: memcpy(&value, src, sizeof(value)); SAY("param:%u:%s:%u", i, p->name, value); break;
            case REAL:
                memcpy(&value_real, src, sizeof(value_real));
                if(!real_text(value_real, a)) return CAPE_E_RANGE;
                SAY("param:%u:%s:%s", i, p->name, a); break;
            case BOOL: SAY("param:%u:%s:%u", i, p->name, *(const uint8_t*)src); break;
            case SPREAD:
                if(*(const uint8_t*)src >= 4) return CAPE_E_RANGE;
                SAY("param:%u:%s:%s", i, p->name, spreads[*(const uint8_t*)src]); break;
            case COLOURS: SAY("param:%u:%s:%08x:%08x", i, p->name, e->colours[0], e->colours[1]); break;
            }
        }
        SAY("lend:%u", i);
    }
    return CAPE_OK;
}
#undef SAY
#undef TRY

static int validate_word(void* context, const char* text){
    return cape_lightmodel_read_word(context, text);
}

int cape_lightmodel_emit(const cape_lightmodel* model, const char* txn, cape_lightmodel_word emit, void* context){
    if(!model || !emit || model->count > COLLECTION_MAX || (!model->layers && model->count) ||
       (model->known != 0 && model->known != 1) || (!model->known && model->count)) return CAPE_E_ARG;
    if(txn){ uint32_t value; if(!hex(txn, 8, &value) || !value) return CAPE_E_ARG; }
    for(uint32_t i = 0; i < model->count; ++i){
        const cape_profile_layer* l = &model->layers[i];
        if(l->nkeys > COLLECTION_MAX || (!l->keys && l->nkeys) || l->effect.ntransitions > COLLECTION_MAX ||
           (!l->effect.transitions && l->effect.ntransitions) || l->effect.kind < 0 || l->effect.kind >= CAPE_PROFILE_EFFECTS)
            return CAPE_E_RANGE;
    }
    // Use the one strict decoder as the structural validator; no user callback sees a partial invalid model.
    cape_lightmodel_reader* reader = cape_lightmodel_reader_new();
    if(!reader) return CAPE_E_CAP;
    writer validation = { validate_word, reader, NULL };
    int rc = emit_model(model, &validation);
    if(rc == CAPE_OK && !cape_lightmodel_ready(reader)) rc = CAPE_E_RANGE;
    cape_lightmodel_reader_free(reader);
    if(rc != CAPE_OK) return rc;
    writer output = { emit, context, txn };
    return emit_model(model, &output);
}
