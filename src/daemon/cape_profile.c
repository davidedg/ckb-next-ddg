#include "cape_profile.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* cape_profile_strerror(int err){
    switch(err){
    case CAPE_PROFILE_E_UNKNOWN:
        return "a profile of an unsupported iCUE";
    case CAPE_PROFILE_E_CORRUPT:
        return "not a profile of iCUE";
    case CAPE_PROFILE_E_MEMORY:
        return "out of memory";
    default:
        return cape_xml_strerror(err);
    }
}

// The classes of the level, with their versions. poly: registered as a polymorphic type with its name (the others, when behind a
// pointer, have the static type 0x40000000)
typedef enum {
    C_BACKUP_PROFILE, C_PROFILE_PROPERTY, C_CHANNEL_BASE, C_LIGHTINGS, C_BRIGHTNESS_IND, C_WINLOCK_IND, C_PROFILE_IND, C_WINLOCK,
    C_SIDE_LIGHTING, C_HW_ACTIONS, C_ACTIONS_BASE, C_PRESETS, C_KEY_ASSOCIATION, C_BASE_ACTION, C_REPEAT, C_EXEC_HINTS,
    C_KEY_REMAP, C_MACRO, C_TEXT, C_MEDIA, C_EVENT_BASE, C_DELAY_EVENT, C_KEY_EVENT, C_LAYER, C_LIGHTING_LAYER, C_LIGHTING_HINTS,
    C_ANIMATION, C_TRANSITIONS, C_TRANSITION,
    C_STATIC, C_SOLID, C_GRADIENT, C_WAVE, C_RIPPLE, C_COLOR_PULSE, C_COLOR_SHIFT, C_COLOR_WAVE, C_RAIN, C_RAINBOW_WAVE,
    C_SPIRAL_RAINBOW, C_TYPE, C_VISOR,
    C_COUNT
} klass;

static const struct {
    const char* name;
    unsigned version;
    unsigned poly;
} classes[C_COUNT] = {
    [C_BACKUP_PROFILE] = { "BackupProfile", 203, 0 },
    [C_PROFILE_PROPERTY] = { "ProfileProperty", 200, 0 },
    [C_CHANNEL_BASE] = { "ChannelPropertiesBase", 200, 0 },
    [C_LIGHTINGS] = { "AdvancedLightingsProperty", 200, 1 },
    [C_BRIGHTNESS_IND] = { "BrightnessIndicatorProperty", 200, 0 },
    [C_WINLOCK_IND] = { "WinlockIndicatorProperty", 400, 0 },
    [C_PROFILE_IND] = { "ProfileIndicatorProperty", 400, 0 },
    [C_WINLOCK] = { "WinlockProperty", 300, 0 },
    [C_SIDE_LIGHTING] = { "SideLightingProperty", 200, 0 },
    [C_HW_ACTIONS] = { "HardwareActionsProperty", 300, 0 },
    [C_ACTIONS_BASE] = { "ActionsBase", 303, 0 },
    [C_PRESETS] = { "ActionsPresets", 400, 0 },
    [C_KEY_ASSOCIATION] = { "KeyAssociation", 400, 0 },
    [C_BASE_ACTION] = { "BaseAction", 202, 0 },
    [C_REPEAT] = { "RepeatOptions", 300, 0 },
    [C_EXEC_HINTS] = { "ExecutionHints", 201, 0 },
    [C_KEY_REMAP] = { "KeyRemapAction", 400, 1 },
    [C_MACRO] = { "MacroAction", 201, 1 },
    [C_TEXT] = { "TextAction", 201, 1 },
    [C_MEDIA] = { "MediaAction", 200, 1 },
    [C_EVENT_BASE] = { "MacroActionEvent", 200, 0 },
    [C_DELAY_EVENT] = { "DelayMacroActionEvent", 200, 1 },
    [C_KEY_EVENT] = { "KeyboardMacroActionEvent", 200, 1 },
    [C_LAYER] = { "AdvancedLightingLayer", 200, 1 },
    [C_LIGHTING_LAYER] = { "LightingLayer", 400, 0 },
    [C_LIGHTING_HINTS] = { "LightingExecutionHints", 200, 0 },
    [C_ANIMATION] = { "CUEAnimationBase", 300, 0 },
    [C_TRANSITIONS] = { "CUEAnimationWithTransitions", 201, 0 },
    [C_TRANSITION] = { "Transition", 200, 0 },
    [C_STATIC] = { "StaticLighting", 201, 1 },
    [C_SOLID] = { "SolidLighting", 201, 1 },
    [C_GRADIENT] = { "GradientLighting", 300, 1 },
    [C_WAVE] = { "WaveLighting", 201, 1 },
    [C_RIPPLE] = { "RippleLighting", 201, 1 },
    [C_COLOR_PULSE] = { "ColorPulseLighting", 300, 1 },
    [C_COLOR_SHIFT] = { "ColorShiftLighting", 300, 1 },
    [C_COLOR_WAVE] = { "ColorWaveLighting", 300, 1 },
    [C_RAIN] = { "RainLighting", 301, 1 },
    [C_RAINBOW_WAVE] = { "RainbowWaveLighting", 301, 1 },
    [C_SPIRAL_RAINBOW] = { "SpiralRainbowLighting", 301, 1 },
    [C_TYPE] = { "TypeLighting", 401, 1 },
    [C_VISOR] = { "VisorLighting", 301, 1 },
};

// The polymorphic types that can stand behind each kind of pointer
static const klass actions[] = { C_KEY_REMAP, C_MACRO, C_TEXT, C_MEDIA };
static const klass events[] = { C_DELAY_EVENT, C_KEY_EVENT };
static const klass effects[] = {
    C_STATIC, C_SOLID, C_GRADIENT, C_WAVE, C_RIPPLE, C_COLOR_PULSE, C_COLOR_SHIFT, C_COLOR_WAVE, C_RAIN, C_RAINBOW_WAVE,
    C_SPIRAL_RAINBOW, C_TYPE, C_VISOR,
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Every polymorphic name iCUE's archive registers that the level does not have: CAPE_PROFILE_E_UNKNOWN rather than corrupt (the
// level is a subset of iCUE's classes; any other name is unknown too)

#define POLY_NEW     0x80000000u
#define POLY_STATIC  0x40000000u
#define ID_MASK      0x3fffffffu

// Enums: their names, in the order of their values
static const char* const e_event[] = { "Click", "LongPress", "ExtraLongPress", "5sLongPress", NULL };
static const char* const e_layer[] = { "StandardLayer", "FunctionalLayer1", "FunctionalLayer2", "CtrlLayer", "AltLayer", "ShiftLayer", NULL };
static const char* const e_hint[] = {
    "OnBoth", "OnPress", "OnRelease", "OnPressLong", "OnPressExtraLong", "OnPress5sLong", "WhilePressed", "Toggle", NULL
};
static const char* const e_repeat[] = { "NoRepeat", "Repeat", "RepeatConstantly", NULL };
static const char* const e_delay[] = { "Constant", "Random", NULL };
static const char* const e_hold[] = { "OnPress", "Toggle", "WhilePressed", NULL };
static const char* const e_sniper[] = { "OnOff", "WhilePressed", NULL };
static const char* const e_terminate[] = { "ExecuteUninterrupted", "AllowInterruptionsWithResume", "Interrupt", NULL };
static const char* const e_media[] = { "PlayPause", "Stop", "PrevTrack", "NextTrack", "VolumeUp", "VolumeDown", "Mute", NULL };
static const char* const e_sub[] = { "KeyPress", "KeyRelease", "KeyStroke", NULL };
static const char* const e_option[] = { "ConcreteLeds", "AllLeds", NULL };
static const char* const e_play[] = { "Standard", "PlayFromKeyCenter", "PlayOnKeyCenter", NULL };
static const char* const e_spread[] = { "OnlyBetweenFronts", "WithAfterInnerFront", "WithBeforeOuterFront", "WithEveryDirection", NULL };

static unsigned enum_count(const char* const* e){
    unsigned n = 0;
    while(e[n])
        n++;
    return n;
}

// Numbers in the C notation, whatever the locale

// The decimal point of the locale of the process, when it is not '.'
static const char* locale_point(void){
    const struct lconv* lc = localeconv();
    return lc && lc->decimal_point && strcmp(lc->decimal_point, ".") != 0 ? lc->decimal_point : NULL;
}

// A real with `digits` significant digits, as printf's %g does (iCUE's archive: 17 for its reals, 6 for the components of a colour)
static size_t format_real(char* out, size_t cap, double v, int digits){
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.*g", digits, v);
    if(n < 0 || (size_t)n >= sizeof(tmp))
        return 0;
    const char* point = locale_point();
    size_t w = 0;
    for(const char* p = tmp; *p && w + 1 < cap;){
        if(point && !strncmp(p, point, strlen(point))){
            out[w++] = '.';
            p += strlen(point);
        } else
            out[w++] = *p++;
    }
    out[w] = 0;
    return w;
}

// A real written as -?digits(.digits)?([eE][+-]?digits)?, finite. 1 if it is one.
static int parse_real(const char* s, size_t n, double* v){
    size_t i = 0;
    if(i < n && s[i] == '-')
        i++;
    const size_t int_start = i;
    while(i < n && s[i] >= '0' && s[i] <= '9')
        i++;
    if(i == int_start)
        return 0;
    if(i < n && s[i] == '.'){
        const size_t frac = ++i;
        while(i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if(i == frac)
            return 0;
    }
    if(i < n && (s[i] == 'e' || s[i] == 'E')){
        i++;
        if(i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        const size_t exp = i;
        while(i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if(i == exp)
            return 0;
    }
    if(i != n || n > 40)
        return 0;
    char tmp[64];
    const char* point = locale_point();
    size_t w = 0;
    for(size_t k = 0; k < n; k++){
        if(s[k] == '.' && point){
            const size_t pl = strlen(point);
            if(w + pl >= sizeof(tmp))
                return 0;
            memcpy(tmp + w, point, pl);
            w += pl;
        } else
            tmp[w++] = s[k];
    }
    tmp[w] = 0;
    char* end;
    *v = strtod(tmp, &end);
    return *end == 0 && isfinite(*v);
}

// An unsigned integer written as decimal, with no leading zero or sign, at most max
static int parse_uint(const char* s, size_t n, uint64_t max, uint64_t* v){
    if(n == 0 || n > 20 || (n > 1 && s[0] == '0'))
        return 0;
    uint64_t x = 0;
    for(size_t i = 0; i < n; i++){
        if(s[i] < '0' || s[i] > '9')
            return 0;
        if(x > (UINT64_MAX - 9) / 10)
            return 0;
        x = x * 10 + (uint64_t)(s[i] - '0');
    }
    if(x > max)
        return 0;
    *v = x;
    return 1;
}

static int parse_int32(const char* s, size_t n, int32_t* v){
    uint64_t x;
    if(n && s[0] == '-'){
        if(!parse_uint(s + 1, n - 1, 0x80000000u, &x) || x == 0)
            return 0;   // (no -0)
        *v = (int32_t)(-(int64_t)x);
        return 1;
    }
    if(!parse_uint(s, n, 0x7fffffffu, &x))
        return 0;
    *v = (int32_t)x;
    return 1;
}

static int hex_digit(char c){
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}, lowercase as QUuid::toString writes it
static int parse_uuid(const char* s, size_t n, cape_profile_uuid* u){
    if(n != 38 || s[0] != '{' || s[37] != '}')
        return 0;
    size_t b = 0;
    for(size_t i = 1; i < 37;){
        if(i == 9 || i == 14 || i == 19 || i == 24){
            if(s[i] != '-')
                return 0;
            i++;
            continue;
        }
        const int hi = hex_digit(s[i]), lo = hex_digit(s[i + 1]);
        if(hi < 0 || lo < 0)
            return 0;
        u->b[b++] = (uint8_t)(hi << 4 | lo);
        i += 2;
    }
    return 1;
}

static void format_uuid(const cape_profile_uuid* u, char out[39]){
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    out[w++] = '{';
    for(int i = 0; i < 16; i++){
        if(i == 4 || i == 6 || i == 8 || i == 10)
            out[w++] = '-';
        out[w++] = hex[u->b[i] >> 4];
        out[w++] = hex[u->b[i] & 15];
    }
    out[w++] = '}';
    out[w] = 0;
}

static int uuid_null(const cape_profile_uuid* u){
    for(int i = 0; i < 16; i++)
        if(u->b[i])
            return 0;
    return 1;
}

// rgb(r g b) or rgba(r g b a), one space between the components, each 0..1
static int parse_colour(const char* s, size_t n, cape_profile_colour* c){
    size_t i;
    unsigned want;
    if(n > 5 && !memcmp(s, "rgba(", 5)){
        i = 5;
        want = 4;
    } else if(n > 4 && !memcmp(s, "rgb(", 4)){
        i = 4;
        want = 3;
    } else
        return 0;
    if(s[n - 1] != ')')
        return 0;
    memset(c, 0, sizeof(*c));
    c->has_alpha = want == 4;
    for(unsigned k = 0; k < want; k++){
        const size_t start = i;
        while(i < n - 1 && s[i] != ' ')
            i++;
        if(!parse_real(s + start, i - start, &c->c[k]) || c->c[k] < 0 || c->c[k] > 1)
            return 0;
        if(k + 1 < want){
            if(i >= n - 1 || s[i] != ' ')
                return 0;
            i++;
        }
    }
    return i == n - 1;
}

static size_t format_colour(const cape_profile_colour* c, char* out, size_t cap){
    // Match QColor's float qFuzzyCompare(alphaF() + 1, 2) when choosing the archive spelling.
    const float shifted_alpha = (float)(c->has_alpha ? c->c[3] : 1.) + 1.f;
    const int has_alpha = c->has_alpha && fminf(fabsf(shifted_alpha), 2.f) < fabsf(shifted_alpha - 2.f) * 100000.f;
    size_t w = (size_t)snprintf(out, cap, has_alpha ? "rgba(" : "rgb(");
    const unsigned n = has_alpha ? 4 : 3;
    for(unsigned k = 0; k < n && w < cap; k++){
        if(k)
            out[w++] = ' ';
        w += format_real(out + w, cap - w, c->c[k], 6);
    }
    if(w + 2 > cap)
        return 0;
    out[w++] = ')';
    out[w] = 0;
    return w;
}

static int colour_ok(const cape_profile_colour* c){
    for(unsigned k = 0; k < (c->has_alpha ? 4u : 3u); k++)
        if(!isfinite(c->c[k]) || c->c[k] < 0 || c->c[k] > 1)
            return 0;
    return 1;
}

// A key name of iCUE: letters, digits, '_' (empty only where a remap has a keystroke instead)
static int key_ok(const char* s, size_t n, int empty_ok){
    if(!n)
        return empty_ok;
    for(size_t i = 0; i < n; i++){
        const char c = s[i];
        if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

// Identities: the UUIDs of actions, layers and animations, never null and never twice in a document
typedef struct {
    cape_profile_uuid* v;
    size_t n, cap;
} id_set;

static int id_add(id_set* s, const cape_profile_uuid* u){
    if(uuid_null(u))
        return CAPE_PROFILE_E_CORRUPT;
    for(size_t i = 0; i < s->n; i++)
        if(!memcmp(s->v[i].b, u->b, 16))
            return CAPE_PROFILE_E_CORRUPT;
    if(s->n == s->cap){
        const size_t cap = s->cap ? s->cap * 2 : 64;
        cape_profile_uuid* v = realloc(s->v, cap * sizeof(*v));
        if(!v)
            return CAPE_PROFILE_E_MEMORY;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = *u;
    return CAPE_OK;
}

// Model memory

static void text_free(cape_profile_text* t){
    free(t->p);
    t->p = NULL;
    t->len = 0;
}

static int text_set(cape_profile_text* t, const char* s, size_t n){
    t->p = NULL;
    t->len = 0;
    if(!n)
        return CAPE_OK;
    t->p = malloc(n + 1);
    if(!t->p)
        return CAPE_PROFILE_E_MEMORY;
    memcpy(t->p, s, n);
    t->p[n] = 0;
    t->len = (uint32_t)n;
    return CAPE_OK;
}

static void texts_free(cape_profile_text* v, uint32_t n){
    for(uint32_t i = 0; i < n; i++)
        text_free(&v[i]);
    free(v);
}

static void action_free(cape_profile_action* a){
    text_free(&a->name);
    text_free(&a->sound_path);
    text_free(&a->key_name);
    texts_free(a->stroke, a->nstroke);
    text_free(&a->program_path);
    for(uint32_t i = 0; i < a->nevents; i++)
        texts_free(a->events[i].keys, a->events[i].nkeys);
    free(a->events);
    text_free(&a->text);
    memset(a, 0, sizeof(*a));
}

static void layer_free(cape_profile_layer* l){
    text_free(&l->name);
    texts_free(l->keys, l->nkeys);
    text_free(&l->effect.name);
    free(l->effect.transitions);
    memset(l, 0, sizeof(*l));
}

void cape_profile_free(cape_profile_profile* p){
    if(!p)
        return;
    for(uint32_t i = 0; i < p->nlayers; i++)
        layer_free(&p->layers[i]);
    free(p->layers);
    for(uint32_t i = 0; i < p->nbindings; i++){
        action_free(&p->bindings[i].action);
        text_free(&p->bindings[i].key);
    }
    free(p->bindings);
    memset(p, 0, sizeof(*p));
}

// Reading

typedef struct {
    const cape_xml_tree* t;
    uint8_t versioned[C_COUNT];   // classes whose version has been read
    klass types[C_COUNT + 1];     // the polymorphic types registered so far, by number (from 1)
    unsigned ntypes;
    uint32_t next_object;         // the number the next shared object must have
    id_set ids;
    char* why;
} rd;

// Fails at a node: the path to it (its last part, if long) and what is wrong
static int rfail(rd* r, uint32_t node, int code, const char* fmt, ...){
    if(!r->why)
        return code;
    char path[512];
    size_t n = sizeof(path) - 1;
    path[n] = 0;
    for(uint32_t i = node; i != CAPE_XML_NONE && i < r->t->count; i = r->t->nodes[i].parent){
        const cape_xml_node* x = &r->t->nodes[i];
        if(n < (size_t)x->name_len + 1)
            break;
        n -= x->name_len;
        memcpy(path + n, r->t->text + x->name, x->name_len);
        if(x->parent != CAPE_XML_NONE)
            path[--n] = '/';
    }
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // the path and the message; when too long, the end of the path (the part that says where) is kept
    char full[sizeof(path) + sizeof(msg) + 4];
    const int len = snprintf(full, sizeof(full), "%s: %s", path + n, msg);
    if(len < 0)
        r->why[0] = 0;
    else if((size_t)len < CAPE_PROFILE_WHY)
        memcpy(r->why, full, (size_t)len + 1);
    else {
        memcpy(r->why, "...", 3);
        memcpy(r->why + 3, full + (size_t)len - (CAPE_PROFILE_WHY - 4), CAPE_PROFILE_WHY - 3);
    }
    return code;
}

static const char* rtext(const rd* r, uint32_t node){
    return r->t->text + r->t->nodes[node].value;
}

static int is_named(const rd* r, uint32_t node, const char* name){
    return cape_xml_is(r->t, node, name);
}

// The fields of a class, in order
typedef struct {
    rd* r;
    uint32_t node;
    uint32_t cur;
} fields;

// Begins the fields of a class at node: its version, the first time the document has the class
static int fields_begin(rd* r, uint32_t node, klass k, fields* f){
    const cape_xml_node* n = &r->t->nodes[node];
    if(n->flags & CAPE_XML_VALUE)
        return rfail(r, node, CAPE_PROFILE_E_CORRUPT, "a value where %s is expected", classes[k].name);
    f->r = r;
    f->node = node;
    f->cur = n->first;
    const int has_version = f->cur != CAPE_XML_NONE && is_named(r, f->cur, "cereal_class_version");
    if(!r->versioned[k]){
        if(!has_version)
            return rfail(r, node, CAPE_PROFILE_E_CORRUPT, "the first %s has no version", classes[k].name);
        uint64_t v;
        const cape_xml_node* vn = &r->t->nodes[f->cur];
        if(!(vn->flags & CAPE_XML_VALUE) || !parse_uint(rtext(r, f->cur), vn->value_len, 0xffffffffu, &v))
            return rfail(r, f->cur, CAPE_PROFILE_E_CORRUPT, "not a version");
        if(v != classes[k].version)
            return rfail(r, f->cur, CAPE_PROFILE_E_UNKNOWN, "%s version %u, the level has %u", classes[k].name, (unsigned)v,
                         classes[k].version);
        r->versioned[k] = 1;
        f->cur = vn->next;
    } else if(has_version)
        return rfail(r, f->cur, CAPE_PROFILE_E_CORRUPT, "%s has its version again", classes[k].name);
    return CAPE_OK;
}

// The next field, which must be named tag
static int field(fields* f, const char* tag, uint32_t* node){
    if(f->cur == CAPE_XML_NONE)
        return rfail(f->r, f->node, CAPE_PROFILE_E_CORRUPT, "no %s", tag);
    if(!is_named(f->r, f->cur, tag))
        return rfail(f->r, f->cur, CAPE_PROFILE_E_CORRUPT, "%s expected here", tag);
    *node = f->cur;
    f->cur = f->r->t->nodes[f->cur].next;
    return CAPE_OK;
}

static int fields_end(fields* f){
    if(f->cur != CAPE_XML_NONE)
        return rfail(f->r, f->cur, CAPE_PROFILE_E_CORRUPT, "a field the class does not have");
    return CAPE_OK;
}

// A value: the node holds a value and nothing else
static int rvalue(rd* r, uint32_t node, const char** s, size_t* n){
    const cape_xml_node* x = &r->t->nodes[node];
    if(!(x->flags & CAPE_XML_VALUE) || x->first != CAPE_XML_NONE)
        return rfail(r, node, CAPE_PROFILE_E_CORRUPT, "not a value");
    *s = rtext(r, node);
    *n = x->value_len;
    return CAPE_OK;
}

#define TRY(x) do { int rc_ = (x); if(rc_ != CAPE_OK) return rc_; } while(0)

static int r_bool(fields* f, const char* tag, uint8_t* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(n == 4 && !memcmp(s, "true", 4))
        *v = 1;
    else if(n == 5 && !memcmp(s, "false", 5))
        *v = 0;
    else
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not true or false");
    return CAPE_OK;
}

static int r_u32(fields* f, const char* tag, uint32_t* v){
    uint32_t node;
    const char* s;
    size_t n;
    uint64_t x;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(!parse_uint(s, n, 0xffffffffu, &x))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not an unsigned integer");
    *v = (uint32_t)x;
    return CAPE_OK;
}

static int r_i32(fields* f, const char* tag, int32_t* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(!parse_int32(s, n, v))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not an integer");
    return CAPE_OK;
}

static int r_real(fields* f, const char* tag, double* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(!parse_real(s, n, v))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not a number");
    return CAPE_OK;
}

static int r_text(fields* f, const char* tag, cape_profile_text* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    return text_set(v, s, n);
}

static int r_uuid(fields* f, const char* tag, cape_profile_uuid* v, int identity){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(!parse_uuid(s, n, v))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not a UUID");
    if(identity){
        const int rc = id_add(&f->r->ids, v);
        if(rc != CAPE_OK)
            return rfail(f->r, node, rc, rc == CAPE_PROFILE_E_MEMORY ? "out of memory" : "a null identity, or one used twice");
    }
    return CAPE_OK;
}

static int r_colour(fields* f, const char* tag, cape_profile_colour* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    if(!parse_colour(s, n, v))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not a colour");
    return CAPE_OK;
}

static int r_enum(fields* f, const char* tag, const char* const* names, uint8_t* v){
    uint32_t node;
    const char* s;
    size_t n;
    TRY(field(f, tag, &node));
    TRY(rvalue(f->r, node, &s, &n));
    for(unsigned i = 0; names[i]; i++)
        if(strlen(names[i]) == n && !memcmp(names[i], s, n)){
            *v = (uint8_t)i;
            return CAPE_OK;
        }
    return rfail(f->r, node, CAPE_PROFILE_E_UNKNOWN, "a value the level does not have");
}

// <tag><nullopt>true</nullopt></tag>, or <tag><nullopt>false</nullopt><data>colour</data></tag>
static int r_optcolour(fields* f, const char* tag, cape_profile_optcolour* v){
    uint32_t node;
    TRY(field(f, tag, &node));
    fields g = { f->r, node, f->r->t->nodes[node].first };
    if(f->r->t->nodes[node].flags & CAPE_XML_VALUE)
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "a value where an optional colour is expected");
    uint8_t null;
    TRY(r_bool(&g, "nullopt", &null));
    memset(v, 0, sizeof(*v));
    if(!null){
        TRY(r_colour(&g, "data", &v->colour));
        v->present = 1;
    }
    return fields_end(&g);
}

// A dynamic collection: its node, and how many items (value0, value1... in order)
static int r_collection(fields* f, const char* tag, uint32_t* node, uint32_t* n){
    TRY(field(f, tag, node));
    rd* r = f->r;
    const cape_xml_node* x = &r->t->nodes[*node];
    if(!(x->flags & CAPE_XML_DYNAMIC))
        return rfail(r, *node, CAPE_PROFILE_E_CORRUPT, "not a collection");
    uint32_t i = 0;
    for(uint32_t c = x->first; c != CAPE_XML_NONE; c = r->t->nodes[c].next, i++){
        char name[16];
        snprintf(name, sizeof(name), "value%u", (unsigned)i);
        if(!is_named(r, c, name))
            return rfail(r, c, CAPE_PROFILE_E_CORRUPT, "%s expected here", name);
    }
    *n = i;
    return CAPE_OK;
}

// A collection of key names (keys of an event, keystroke of a remap, lights of a layer)
static int r_keys(fields* f, const char* tag, cape_profile_text** out, uint32_t* nout){
    uint32_t node, n;
    TRY(r_collection(f, tag, &node, &n));
    *out = NULL;
    *nout = 0;
    if(!n)
        return CAPE_OK;
    cape_profile_text* v = calloc(n, sizeof(*v));
    if(!v)
        return CAPE_PROFILE_E_MEMORY;
    *out = v;
    rd* r = f->r;
    uint32_t c = r->t->nodes[node].first;
    for(uint32_t i = 0; i < n; i++, c = r->t->nodes[c].next){
        const char* s;
        size_t len;
        TRY(rvalue(r, c, &s, &len));
        if(!key_ok(s, len, 0))
            return rfail(r, c, CAPE_PROFILE_E_CORRUPT, "not a key name");
        TRY(text_set(&v[i], s, len));
        *nout = i + 1;
    }
    return CAPE_OK;
}

// A pointer: its type, checked against what may stand there, and the node of its data. unique: <valid>1</valid> (the root
// properties), else a shared pointer, which must be a new object (no object is shared in the level)
static int r_pointer(fields* f, const char* tag, const klass* allowed, size_t nallowed, int unique, klass* k, uint32_t* data){
    uint32_t node;
    TRY(field(f, tag, &node));
    rd* r = f->r;
    fields g = { r, node, r->t->nodes[node].first };
    if(r->t->nodes[node].flags & CAPE_XML_VALUE)
        return rfail(r, node, CAPE_PROFILE_E_CORRUPT, "a value where a pointer is expected");
    uint32_t idn, raw;
    TRY(r_u32(&g, "polymorphic_id", &raw));
    idn = r->t->nodes[node].first;
    if(raw == POLY_STATIC){
        if(nallowed != 1 || classes[allowed[0]].poly)
            return rfail(r, idn, CAPE_PROFILE_E_CORRUPT, "a static type where a polymorphic one is expected");
        *k = allowed[0];
    } else if(raw & POLY_STATIC)
        return rfail(r, idn, CAPE_PROFILE_E_CORRUPT, "not a type number");
    else if(raw & POLY_NEW){
        uint32_t nn;
        const char* s;
        size_t n;
        TRY(field(&g, "polymorphic_name", &nn));
        TRY(rvalue(r, nn, &s, &n));
        if((raw & ID_MASK) != r->ntypes + 1)
            return rfail(r, idn, CAPE_PROFILE_E_CORRUPT, "a new type out of order");
        klass found = C_COUNT;
        for(unsigned c = 0; c < C_COUNT; c++)
            if(classes[c].poly && strlen(classes[c].name) == n && !memcmp(classes[c].name, s, n))
                found = (klass)c;
        if(found == C_COUNT)
            return rfail(r, nn, CAPE_PROFILE_E_UNKNOWN, "a type the level does not have");
        for(unsigned i = 1; i <= r->ntypes; i++)
            if(r->types[i] == found)
                return rfail(r, nn, CAPE_PROFILE_E_CORRUPT, "a type registered twice");
        r->types[++r->ntypes] = found;
        *k = found;
    } else {
        if(raw == 0 || raw > r->ntypes)
            return rfail(r, idn, CAPE_PROFILE_E_CORRUPT, "a type that is not registered");
        *k = r->types[raw];
    }
    int ok = 0;
    for(size_t i = 0; i < nallowed; i++)
        ok |= allowed[i] == *k;
    if(!ok)
        return rfail(r, idn, CAPE_PROFILE_E_CORRUPT, "%s cannot stand here", classes[*k].name);
    uint32_t wrapper;
    TRY(field(&g, "ptr_wrapper", &wrapper));
    TRY(fields_end(&g));
    fields w = { r, wrapper, r->t->nodes[wrapper].first };
    if(unique){
        uint32_t valid;
        const char* s;
        size_t n;
        TRY(field(&w, "valid", &valid));
        TRY(rvalue(r, valid, &s, &n));
        if(n != 1 || s[0] != '1')
            return rfail(r, valid, CAPE_PROFILE_E_CORRUPT, "not a valid pointer");
    } else {
        uint32_t id;
        TRY(r_u32(&w, "id", &id));
        if(w.cur == CAPE_XML_NONE)
            return rfail(r, wrapper, CAPE_PROFILE_E_UNKNOWN, "an object shared by two pointers");
        if(id != (POLY_NEW | r->next_object))
            return rfail(r, wrapper, CAPE_PROFILE_E_CORRUPT, "an object out of order");
        r->next_object++;
    }
    TRY(field(&w, "data", data));
    return fields_end(&w);
}

// The base of every root property: an empty ProfileProperty
static int r_profile_property(fields* f){
    uint32_t node;
    fields g;
    TRY(field(f, "base", &node));
    TRY(fields_begin(f->r, node, C_PROFILE_PROPERTY, &g));
    return fields_end(&g);
}

static int r_event(rd* r, uint32_t data, klass k, cape_profile_event* e){
    fields f, g;
    uint32_t base;
    TRY(fields_begin(r, data, k, &f));
    TRY(field(&f, "base", &base));
    TRY(fields_begin(r, base, C_EVENT_BASE, &g));
    TRY(fields_end(&g));
    if(k == C_DELAY_EVENT){
        e->kind = CAPE_PROFILE_DELAY_EVENT;
        TRY(r_bool(&f, "useRandom", &e->random));
        if(e->random){
            TRY(r_u32(&f, "delayMin", &e->delay_min));
            TRY(r_u32(&f, "delayMax", &e->delay_max));
        } else
            TRY(r_u32(&f, "delay", &e->delay));
    } else {
        e->kind = CAPE_PROFILE_KEY_EVENT;
        TRY(r_keys(&f, "keys", &e->keys, &e->nkeys));
        TRY(r_enum(&f, "sub", e_sub, &e->sub));
    }
    return fields_end(&f);
}

static int r_action(rd* r, uint32_t data, klass k, cape_profile_action* a){
    fields f, b, g;
    uint32_t node, n;
    TRY(fields_begin(r, data, k, &f));
    TRY(field(&f, "base", &node));
    TRY(fields_begin(r, node, C_BASE_ACTION, &b));
    TRY(r_text(&b, "name", &a->name));
    TRY(r_uuid(&b, "id", &a->id, 1));
    TRY(field(&b, "repeatOptions", &node));
    TRY(fields_begin(r, node, C_REPEAT, &g));
    TRY(r_u32(&g, "repeatCount", &a->repeat_count));
    TRY(r_enum(&g, "repeatMode", e_repeat, &a->repeat_mode));
    TRY(r_u32(&g, "delay", &a->repeat_delay));
    TRY(r_enum(&g, "delayMode", e_delay, &a->delay_mode));
    TRY(r_u32(&g, "randomDelayFrom", &a->random_delay_from));
    TRY(r_u32(&g, "randomDelayTo", &a->random_delay_to));
    TRY(fields_end(&g));
    TRY(field(&b, "executionHints", &node));
    TRY(fields_begin(r, node, C_EXEC_HINTS, &g));
    TRY(r_bool(&g, "terminateOnSecondExec", &a->terminate_on_second));
    TRY(r_bool(&g, "restartOnSecondExec", &a->restart_on_second));
    TRY(r_enum(&g, "execHint", e_hint, &a->exec_hint));
    TRY(r_bool(&g, "retainOriginalKeyOutput", &a->retain_original));
    TRY(fields_end(&g));
    TRY(r_uuid(&b, "actionLighting", &a->lighting, 0));
    TRY(r_text(&b, "actionSoundPath", &a->sound_path));
    TRY(r_collection(&b, "attachedActions", &node, &n));
    if(n)
        return rfail(r, node, CAPE_PROFILE_E_UNKNOWN, "attached actions");
    TRY(fields_end(&b));
    switch(k){
    case C_KEY_REMAP: {
        a->kind = CAPE_PROFILE_REMAP;
        uint32_t kn;
        const char* s;
        size_t len;
        TRY(field(&f, "keyName", &kn));
        TRY(rvalue(r, kn, &s, &len));
        if(!key_ok(s, len, 1))
            return rfail(r, kn, CAPE_PROFILE_E_CORRUPT, "not a key name");
        TRY(text_set(&a->key_name, s, len));
        TRY(r_keys(&f, "keyStroke", &a->stroke, &a->nstroke));
        TRY(r_bool(&f, "holdingKeyEnabled", &a->holding_enabled));
        TRY(r_enum(&f, "holdingKeyType", e_hold, &a->holding_type));
        TRY(r_u32(&f, "holdingKeyOnPressInterval", &a->holding_interval));
        TRY(r_text(&f, "programPath", &a->program_path));
        TRY(r_enum(&f, "sniperSwitchMode", e_sniper, &a->sniper_mode));
        TRY(r_u32(&f, "remapGroup", &a->remap_group));
        break;
    }
    case C_MACRO: {
        a->kind = CAPE_PROFILE_MACRO;
        TRY(r_enum(&f, "terminationMethod", e_terminate, &a->termination));
        TRY(r_bool(&f, "highPriority", &a->high_priority));
        TRY(r_collection(&f, "events", &node, &n));
        if(n){
            a->events = calloc(n, sizeof(*a->events));
            if(!a->events)
                return CAPE_PROFILE_E_MEMORY;
        }
        uint32_t c = r->t->nodes[node].first;
        for(uint32_t i = 0; i < n; i++, c = r->t->nodes[c].next){
            // each item is a pointer named valueN: read as the next field of the collection
            fields one = { r, node, c };
            char name[16];
            snprintf(name, sizeof(name), "value%u", (unsigned)i);
            klass ek;
            uint32_t edata;
            TRY(r_pointer(&one, name, events, COUNT(events), 0, &ek, &edata));
            a->nevents = i + 1;
            TRY(r_event(r, edata, ek, &a->events[i]));
        }
        break;
    }
    case C_TEXT:
        a->kind = CAPE_PROFILE_TEXT;
        TRY(r_text(&f, "text", &a->text));
        TRY(r_u32(&f, "delayBetweenCharacters", &a->char_delay));
        break;
    default:
        a->kind = CAPE_PROFILE_MEDIA;
        TRY(r_enum(&f, "action", e_media, &a->media));
        break;
    }
    return fields_end(&f);
}

// The colours of an effect: <colors><first>ARGB</first><second>ARGB</second></colors>
static int r_colours(fields* f, uint32_t colours[2]){
    uint32_t node;
    TRY(field(f, "colors", &node));
    if(f->r->t->nodes[node].flags & (CAPE_XML_VALUE | CAPE_XML_DYNAMIC))
        return rfail(f->r, node, CAPE_PROFILE_E_CORRUPT, "not a pair of colours");
    fields g = { f->r, node, f->r->t->nodes[node].first };
    TRY(r_u32(&g, "first", &colours[0]));
    TRY(r_u32(&g, "second", &colours[1]));
    return fields_end(&g);
}

// CUEAnimationBase, under the node `tag`
static int r_animation(fields* f, const char* tag, cape_profile_effect* e){
    uint32_t node;
    fields g;
    TRY(field(f, tag, &node));
    TRY(fields_begin(f->r, node, C_ANIMATION, &g));
    TRY(r_text(&g, "name", &e->name));
    TRY(r_uuid(&g, "id", &e->id, 1));
    TRY(r_u32(&g, "duration", &e->duration));
    TRY(r_real(&g, "brightness", &e->brightness));
    return fields_end(&g);
}

// CUEAnimationWithTransitions, under base
static int r_with_transitions(fields* f, cape_profile_effect* e){
    uint32_t node, list, n;
    fields g;
    TRY(field(f, "base", &node));
    TRY(fields_begin(f->r, node, C_TRANSITIONS, &g));
    TRY(r_animation(&g, "base", e));
    TRY(r_collection(&g, "transitions", &list, &n));
    rd* r = f->r;
    if(n){
        e->transitions = calloc(n, sizeof(*e->transitions));
        if(!e->transitions)
            return CAPE_PROFILE_E_MEMORY;
    }
    uint32_t c = r->t->nodes[list].first;
    for(uint32_t i = 0; i < n; i++, c = r->t->nodes[c].next){
        fields t;
        TRY(fields_begin(r, c, C_TRANSITION, &t));
        e->ntransitions = i + 1;
        TRY(r_real(&t, "time", &e->transitions[i].time));
        TRY(r_colour(&t, "color", &e->transitions[i].colour));
        TRY(fields_end(&t));
    }
    return fields_end(&g);
}

int cape_profile_solid_valid(const cape_profile_effect* e){
    if(!e || e->kind != CAPE_PROFILE_SOLID) return 1;
    if(e->ntransitions % 2 || (e->ntransitions && !e->transitions)) return 0;
    for(uint32_t i = 0; i < e->ntransitions; i += 2){
        const cape_profile_colour* a = &e->transitions[i].colour;
        const cape_profile_colour* b = &e->transitions[i + 1].colour;
        for(unsigned c = 0; c < 3; ++c)
            if(a->c[c] != b->c[c]) return 0;
        if((a->has_alpha ? a->c[3] : 1.) != (b->has_alpha ? b->c[3] : 1.)) return 0;
    }
    return 1;
}

const char* cape_profile_layer_error(const cape_profile_layer* l){
    if(!l || (unsigned)l->effect.kind >= CAPE_PROFILE_EFFECTS) return "unknown lighting type";
    const cape_profile_effect* e = &l->effect;
    if(e->ntransitions && !e->transitions) return "missing lighting transitions";
    for(uint32_t i = 0; i < e->ntransitions; ++i){
        const cape_profile_transition* t = &e->transitions[i];
        if(!isfinite(t->time) || t->time < 0 || t->time > 1 || (i && t->time < e->transitions[i - 1].time))
            return "transition time outside iCUE limits";
        for(unsigned c = 0; c < (t->colour.has_alpha ? 4u : 3u); ++c)
            if(!isfinite(t->colour.c[c]) || t->colour.c[c] < 0 || t->colour.c[c] > 1)
                return "transition colour outside iCUE limits";
    }
    if(!cape_profile_solid_valid(e)) return "Solid blocks need paired points of one colour";
    if(l->stop_after_times < -1 || l->stop_after_times > 99) return "repeat count outside iCUE limits";
    if(e->kind >= CAPE_PROFILE_SOLID && e->kind <= CAPE_PROFILE_RIPPLE){
        if(e->duration < 100 || e->duration > 99900 || e->duration % 100)
            return "duration outside iCUE limits (0.1..99.9 s, steps of 0.1 s)";
    }
    if(e->kind == CAPE_PROFILE_WAVE || e->kind == CAPE_PROFILE_RIPPLE){
        if(!isfinite(e->velocity) || e->velocity < 1 || e->velocity > 999 || trunc(e->velocity) != e->velocity ||
           !isfinite(e->tail_length) || e->tail_length < 1 || e->tail_length > 999 || trunc(e->tail_length) != e->tail_length)
            return "tail or velocity outside iCUE limits (0.1..99.9, steps of 0.1)";
    }
    if(e->kind == CAPE_PROFILE_WAVE && (!isfinite(e->angle) || e->angle < 0 || e->angle > 359 || trunc(e->angle) != e->angle))
        return "angle outside iCUE limits (integer 0..359)";
    if((e->kind == CAPE_PROFILE_STATIC || e->kind == CAPE_PROFILE_RAIN || e->kind == CAPE_PROFILE_TYPE) && e->opacity > 255)
        return "opacity outside iCUE limits";
    if(e->kind >= CAPE_PROFILE_COLOR_PULSE && (e->speed < 1 || e->speed > 3)) return "speed outside iCUE limits";
    if((e->kind == CAPE_PROFILE_COLOR_WAVE || e->kind == CAPE_PROFILE_RAINBOW_WAVE) && (e->direction < 1 || e->direction > 4))
        return "direction outside iCUE limits";
    if(e->kind == CAPE_PROFILE_TYPE && (e->type_behaviour > 1 || e->type_duration < 1 || e->type_duration > 3))
        return "type-lighting mode or duration outside iCUE limits";
    return NULL;
}

static int r_effect(rd* r, uint32_t data, klass k, cape_profile_effect* e){
    fields f;
    TRY(fields_begin(r, data, k, &f));
    e->kind = (cape_profile_effect_kind)((int)k - (int)C_STATIC);
    switch(k){
    case C_STATIC:
        TRY(r_with_transitions(&f, e));
        TRY(r_u32(&f, "opacity", &e->opacity));
        break;
    case C_SOLID:
        TRY(r_with_transitions(&f, e));
        if(!cape_profile_solid_valid(e)) return rfail(r, data, CAPE_PROFILE_E_CORRUPT, "Solid blocks need paired points of one colour");
        break;
    case C_GRADIENT:
        TRY(r_with_transitions(&f, e));
        break;
    case C_WAVE:
        TRY(r_with_transitions(&f, e));
        TRY(r_real(&f, "velocity", &e->velocity));
        TRY(r_real(&f, "tailLength", &e->tail_length));
        TRY(r_bool(&f, "isDoublesided", &e->double_sided));
        TRY(r_real(&f, "angle", &e->angle));
        break;
    case C_RIPPLE:
        TRY(r_with_transitions(&f, e));
        TRY(r_real(&f, "velocity", &e->velocity));
        TRY(r_real(&f, "tailLength", &e->tail_length));
        TRY(r_bool(&f, "autoDuration", &e->auto_duration));
        TRY(r_enum(&f, "waveSpread", e_spread, &e->wave_spread));
        TRY(r_colours(&f, e->colours));
        break;
    case C_COLOR_PULSE:
    case C_COLOR_SHIFT:
        TRY(r_animation(&f, "base", e));
        TRY(r_colours(&f, e->colours));
        TRY(r_bool(&f, "random", &e->random));
        TRY(r_u32(&f, "speed", &e->speed));
        break;
    case C_COLOR_WAVE:
        TRY(r_animation(&f, "value0", e));
        TRY(r_u32(&f, "speed", &e->speed));
        TRY(r_bool(&f, "random", &e->random));
        TRY(r_colours(&f, e->colours));
        TRY(r_u32(&f, "direction", &e->direction));
        break;
    case C_RAIN:
        TRY(r_animation(&f, "base", e));
        TRY(r_colours(&f, e->colours));
        TRY(r_bool(&f, "random", &e->random));
        TRY(r_u32(&f, "opacity", &e->opacity));
        TRY(r_u32(&f, "speed", &e->speed));
        TRY(r_u32(&f, "direction", &e->direction));
        break;
    case C_RAINBOW_WAVE:
        TRY(r_animation(&f, "base", e));
        TRY(r_u32(&f, "speed", &e->speed));
        TRY(r_u32(&f, "direction", &e->direction));
        break;
    case C_SPIRAL_RAINBOW:
        TRY(r_animation(&f, "base", e));
        TRY(r_bool(&f, "clockwise", &e->clockwise));
        TRY(r_u32(&f, "speed", &e->speed));
        break;
    case C_TYPE:
        TRY(r_animation(&f, "base", e));
        TRY(r_colours(&f, e->colours));
        TRY(r_bool(&f, "random", &e->random));
        TRY(r_u32(&f, "opacity", &e->opacity));
        TRY(r_u32(&f, "speed", &e->speed));
        TRY(r_u32(&f, "duration", &e->type_duration));
        TRY(r_u32(&f, "typeLightingBehavior", &e->type_behaviour));
        break;
    default:   // C_VISOR
        TRY(r_animation(&f, "base", e));
        TRY(r_colours(&f, e->colours));
        TRY(r_bool(&f, "random", &e->random));
        TRY(r_u32(&f, "speed", &e->speed));
        TRY(r_u32(&f, "direction", &e->direction));
        break;
    }
    return fields_end(&f);
}

static int r_layer(rd* r, uint32_t data, cape_profile_layer* l){
    fields f, b, g;
    uint32_t node;
    TRY(fields_begin(r, data, C_LAYER, &f));
    TRY(field(&f, "base", &node));
    TRY(fields_begin(r, node, C_LIGHTING_LAYER, &b));
    TRY(r_uuid(&b, "id", &l->id, 1));
    TRY(r_text(&b, "name", &l->name));
    TRY(r_keys(&b, "keys", &l->keys, &l->nkeys));
    TRY(r_bool(&b, "enabled", &l->enabled));
    TRY(field(&b, "executionHints", &node));
    TRY(fields_begin(r, node, C_LIGHTING_HINTS, &g));
    TRY(r_bool(&g, "startOnKeyPress", &l->start_on_key_press));
    TRY(r_bool(&g, "startWithProfile", &l->start_with_profile));
    TRY(r_bool(&g, "stopOnKeyPress", &l->stop_on_key_press));
    TRY(r_bool(&g, "stopOnKeyRelease", &l->stop_on_key_release));
    TRY(r_i32(&g, "stopAfterTimes", &l->stop_after_times));
    TRY(r_enum(&g, "playOption", e_play, &l->play_option));
    TRY(fields_end(&g));
    TRY(r_enum(&b, "option", e_option, &l->option));
    TRY(fields_end(&b));
    klass k;
    uint32_t edata;
    TRY(r_pointer(&f, "lighting", effects, COUNT(effects), 0, &k, &edata));
    TRY(r_effect(r, edata, k, &l->effect));
    const char* error = cape_profile_layer_error(l);
    if(error) return rfail(r, data, CAPE_PROFILE_E_CORRUPT, "%s", error);
    return fields_end(&f);
}

static int r_profile(rd* r, uint32_t root, cape_profile_profile* p){
    fields f, d, b, g;
    uint32_t node, data, list, n;
    klass k;
    TRY(fields_begin(r, root, C_BACKUP_PROFILE, &f));
    TRY(r_bool(&f, "isAdvancedMode", &p->advanced));
    // The lighting
    static const klass lightings[] = { C_LIGHTINGS };
    TRY(r_pointer(&f, "LightingsProperty", lightings, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_LIGHTINGS, &d));
    TRY(field(&d, "base", &node));
    TRY(fields_begin(r, node, C_CHANNEL_BASE, &b));
    TRY(r_profile_property(&b));
    TRY(r_collection(&b, "layers", &list, &n));
    if(n){
        p->layers = calloc(n, sizeof(*p->layers));
        if(!p->layers)
            return CAPE_PROFILE_E_MEMORY;
    }
    uint32_t c = r->t->nodes[list].first;
    for(uint32_t i = 0; i < n; i++, c = r->t->nodes[c].next){
        fields one = { r, list, c };
        char name[16];
        snprintf(name, sizeof(name), "value%u", (unsigned)i);
        static const klass layers[] = { C_LAYER };
        uint32_t ldata;
        TRY(r_pointer(&one, name, layers, 1, 0, &k, &ldata));
        p->nlayers = i + 1;
        TRY(r_layer(r, ldata, &p->layers[i]));
    }
    TRY(fields_end(&b));
    TRY(fields_end(&d));
    // The indicators, Win Lock and the side lighting
    static const klass brightness[] = { C_BRIGHTNESS_IND };
    TRY(r_pointer(&f, "BrightnessIndicatorProperty", brightness, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_BRIGHTNESS_IND, &d));
    TRY(r_profile_property(&d));
    TRY(r_colour(&d, "indicatorColor", &p->brightness));
    TRY(fields_end(&d));
    static const klass winlock_ind[] = { C_WINLOCK_IND };
    TRY(r_pointer(&f, "WinlockIndicatorProperty", winlock_ind, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_WINLOCK_IND, &d));
    TRY(r_profile_property(&d));
    TRY(r_optcolour(&d, "enabledIndicatorColor", &p->winlock_on));
    TRY(r_optcolour(&d, "disabledIndicatorColor", &p->winlock_off));
    TRY(fields_end(&d));
    static const klass profile_ind[] = { C_PROFILE_IND };
    TRY(r_pointer(&f, "ProfileIndicatorProperty", profile_ind, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_PROFILE_IND, &d));
    TRY(r_profile_property(&d));
    TRY(r_colour(&d, "m1Color", &p->m1));
    TRY(r_colour(&d, "m2Color", &p->m2));
    TRY(r_colour(&d, "m3Color", &p->m3));
    TRY(r_optcolour(&d, "profileIndicatorColor", &p->profile));
    TRY(fields_end(&d));
    static const klass winlock[] = { C_WINLOCK };
    TRY(r_pointer(&f, "WinlockProperty", winlock, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_WINLOCK, &d));
    TRY(r_profile_property(&d));
    TRY(r_bool(&d, "disableAltTab", &p->disable_alt_tab));
    TRY(r_bool(&d, "disableAltF4", &p->disable_alt_f4));
    TRY(r_bool(&d, "disableWinKey", &p->disable_win));
    TRY(r_bool(&d, "disableShiftTab", &p->disable_shift_tab));
    TRY(r_bool(&d, "disableEncryption", &p->disable_encryption));
    TRY(r_bool(&d, "disableTouchpad", &p->disable_touchpad));
    TRY(r_bool(&d, "swapFKeyFunctions", &p->swap_fkeys));
    TRY(fields_end(&d));
    static const klass side[] = { C_SIDE_LIGHTING };
    TRY(r_pointer(&f, "SideLightingProperty", side, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_SIDE_LIGHTING, &d));
    TRY(r_profile_property(&d));
    TRY(r_bool(&d, "enabled", &p->side_lighting));
    TRY(fields_end(&d));
    // The actions
    static const klass hw[] = { C_HW_ACTIONS };
    TRY(r_pointer(&f, "ActionsProperty", hw, 1, 1, &k, &data));
    TRY(fields_begin(r, data, C_HW_ACTIONS, &d));
    TRY(field(&d, "base", &node));
    TRY(fields_begin(r, node, C_ACTIONS_BASE, &b));
    TRY(r_profile_property(&b));
    TRY(r_collection(&b, "actions", &list, &n));
    if(n){
        p->bindings = calloc(n, sizeof(*p->bindings));
        if(!p->bindings)
            return CAPE_PROFILE_E_MEMORY;
    }
    c = r->t->nodes[list].first;
    for(uint32_t i = 0; i < n; i++, c = r->t->nodes[c].next){
        cape_profile_binding* bd = &p->bindings[i];
        p->nbindings = i + 1;
        if(r->t->nodes[c].flags & CAPE_XML_VALUE)
            return rfail(r, c, CAPE_PROFILE_E_CORRUPT, "not a pair");
        fields pair = { r, c, r->t->nodes[c].first };
        uint32_t adata;
        TRY(r_pointer(&pair, "first", actions, COUNT(actions), 0, &k, &adata));
        TRY(r_action(r, adata, k, &bd->action));
        TRY(field(&pair, "second", &node));
        TRY(fields_begin(r, node, C_KEY_ASSOCIATION, &g));
        uint32_t kn;
        const char* s;
        size_t len;
        TRY(field(&g, "key", &kn));
        TRY(rvalue(r, kn, &s, &len));
        // empty: an action iCUE keeps on no key (its menu of a key, Delete, leaves the action in the list)
        if(!key_ok(s, len, 1))
            return rfail(r, kn, CAPE_PROFILE_E_CORRUPT, "not a key name");
        TRY(text_set(&bd->key, s, len));
        TRY(r_enum(&g, "layer", e_layer, &bd->layer));
        TRY(r_enum(&g, "event", e_event, &bd->event));
        TRY(r_u32(&g, "distance", &bd->distance));
        TRY(fields_end(&g));
        TRY(fields_end(&pair));
    }
    static const klass presets[] = { C_PRESETS };
    TRY(r_pointer(&b, "presetsInfo", presets, 1, 0, &k, &data));
    TRY(fields_begin(r, data, C_PRESETS, &g));
    TRY(r_uuid(&g, "activePresetId", &p->active_preset, 0));
    TRY(r_bool(&g, "isActive", &p->preset_active));
    TRY(fields_end(&g));
    TRY(fields_end(&b));
    TRY(r_u32(&d, "hwActionsSupportedTypes", &p->hw_actions_types));
    TRY(fields_end(&d));
    return fields_end(&f);
}

int cape_profile_read(const cape_xml_tree* t, cape_profile_profile* p, char* why){
    if(!t || !p)
        return CAPE_E_ARG;
    memset(p, 0, sizeof(*p));
    if(why)
        why[0] = 0;
    rd r;
    memset(&r, 0, sizeof(r));
    r.t = t;
    r.next_object = 1;
    r.why = why;
    int rc;
    if(!t->count || !cape_xml_is(t, 0, "cereal"))
        rc = rfail(&r, t->count ? 0 : CAPE_XML_NONE, CAPE_PROFILE_E_CORRUPT, "not a cereal document");
    else {
        fields top = { &r, 0, t->nodes[0].first };
        uint32_t root;
        rc = field(&top, "profile", &root);
        if(rc == CAPE_OK)
            rc = r_profile(&r, root, p);
        if(rc == CAPE_OK)
            rc = fields_end(&top);
    }
    if(rc == CAPE_PROFILE_E_MEMORY && why && !why[0])
        snprintf(why, CAPE_PROFILE_WHY, "out of memory");
    free(r.ids.v);
    if(rc != CAPE_OK)
        cape_profile_free(p);
    return rc;
}

// Writing

typedef struct {
    cape_xml_tree* t;
    uint8_t versioned[C_COUNT];   // classes whose version has been written
    unsigned type_no[C_COUNT];    // the number of each polymorphic type registered so far (0: not yet)
    unsigned ntypes;
    uint32_t next_object;
    id_set ids;
    char* why;
} wr;

static int wfail(wr* w, int code, const char* fmt, ...){
    if(w->why){
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(w->why, CAPE_PROFILE_WHY, fmt, ap);
        va_end(ap);
    }
    return code;
}

static int w_node(wr* w, uint32_t parent, const char* tag, unsigned flags, const char* v, size_t n, uint32_t* out){
    const int rc = cape_xml_add(w->t, parent, tag, flags, v, n);
    if(rc < 0)
        return wfail(w, rc, "%s: %s", tag, cape_xml_strerror(rc));
    if(out)
        *out = (uint32_t)rc;
    return CAPE_OK;
}

static int w_value(wr* w, uint32_t parent, const char* tag, const char* v, size_t n){
    return w_node(w, parent, tag, CAPE_XML_VALUE, v, n, NULL);
}

// A class at a new node, with its version the first time the document has it
static int w_class(wr* w, uint32_t parent, const char* tag, klass k, uint32_t* node){
    TRY(w_node(w, parent, tag, 0, NULL, 0, node));
    if(!w->versioned[k]){
        w->versioned[k] = 1;
        char v[16];
        const int n = snprintf(v, sizeof(v), "%u", classes[k].version);
        TRY(w_value(w, *node, "cereal_class_version", v, (size_t)n));
    }
    return CAPE_OK;
}

static int w_u32(wr* w, uint32_t parent, const char* tag, uint32_t x){
    char v[16];
    const int n = snprintf(v, sizeof(v), "%u", (unsigned)x);
    return w_value(w, parent, tag, v, (size_t)n);
}

static int w_i32(wr* w, uint32_t parent, const char* tag, int32_t x){
    char v[16];
    const int n = snprintf(v, sizeof(v), "%ld", (long)x);
    return w_value(w, parent, tag, v, (size_t)n);
}

static int w_bool(wr* w, uint32_t parent, const char* tag, uint8_t x){
    return x ? w_value(w, parent, tag, "true", 4) : w_value(w, parent, tag, "false", 5);
}

static int w_real(wr* w, uint32_t parent, const char* tag, double x){
    char v[64];
    if(!isfinite(x))
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "%s: not a finite number", tag);
    const size_t n = format_real(v, sizeof(v), x, 17);
    return w_value(w, parent, tag, v, n);
}

static int w_text(wr* w, uint32_t parent, const char* tag, const cape_profile_text* x){
    return w_value(w, parent, tag, x->p, x->len);
}

static int w_uuid(wr* w, uint32_t parent, const char* tag, const cape_profile_uuid* u, int identity){
    if(identity){
        const int rc = id_add(&w->ids, u);
        if(rc != CAPE_OK)
            return wfail(w, rc, rc == CAPE_PROFILE_E_MEMORY ? "out of memory" : "%s: a null identity, or one used twice", tag);
    }
    char v[39];
    format_uuid(u, v);
    return w_value(w, parent, tag, v, 38);
}

static int w_colour(wr* w, uint32_t parent, const char* tag, const cape_profile_colour* c){
    char v[128];
    if(!colour_ok(c))
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "%s: a colour component out of 0..1", tag);
    const size_t n = format_colour(c, v, sizeof(v));
    return w_value(w, parent, tag, v, n);
}

static int w_enum(wr* w, uint32_t parent, const char* tag, const char* const* names, uint8_t x){
    if(x >= enum_count(names))
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "%s: %u is not one of its values", tag, (unsigned)x);
    return w_value(w, parent, tag, names[x], strlen(names[x]));
}

static int w_optcolour(wr* w, uint32_t parent, const char* tag, const cape_profile_optcolour* c){
    uint32_t node;
    TRY(w_node(w, parent, tag, 0, NULL, 0, &node));
    TRY(w_bool(w, node, "nullopt", !c->present));
    if(c->present)
        TRY(w_colour(w, node, "data", &c->colour));
    return CAPE_OK;
}

static int w_collection(wr* w, uint32_t parent, const char* tag, uint32_t* node){
    return w_node(w, parent, tag, CAPE_XML_DYNAMIC, NULL, 0, node);
}

static const char* item_name(char name[16], uint32_t i){
    snprintf(name, 16, "value%u", (unsigned)i);
    return name;
}

static int w_keys(wr* w, uint32_t parent, const char* tag, const cape_profile_text* keys, uint32_t n){
    uint32_t node;
    char name[16];
    TRY(w_collection(w, parent, tag, &node));
    for(uint32_t i = 0; i < n; i++){
        if(!key_ok(keys[i].p, keys[i].len, 0))
            return wfail(w, CAPE_PROFILE_E_CORRUPT, "%s: not a key name", tag);
        TRY(w_value(w, node, item_name(name, i), keys[i].p, keys[i].len));
    }
    return CAPE_OK;
}

// A pointer to a new object of class k, and the node of its data. unique: <valid>1</valid> (the root properties)
static int w_pointer(wr* w, uint32_t parent, const char* tag, klass k, int unique, uint32_t* data){
    uint32_t node, wrapper;
    char v[16];
    TRY(w_node(w, parent, tag, 0, NULL, 0, &node));
    if(classes[k].poly){
        if(!w->type_no[k]){
            w->type_no[k] = ++w->ntypes;
            const int n = snprintf(v, sizeof(v), "%u", (unsigned)(POLY_NEW | w->type_no[k]));
            TRY(w_value(w, node, "polymorphic_id", v, (size_t)n));
            TRY(w_value(w, node, "polymorphic_name", classes[k].name, strlen(classes[k].name)));
        } else {
            const int n = snprintf(v, sizeof(v), "%u", w->type_no[k]);
            TRY(w_value(w, node, "polymorphic_id", v, (size_t)n));
        }
    } else {
        const int n = snprintf(v, sizeof(v), "%u", (unsigned)POLY_STATIC);
        TRY(w_value(w, node, "polymorphic_id", v, (size_t)n));
    }
    TRY(w_node(w, node, "ptr_wrapper", 0, NULL, 0, &wrapper));
    if(unique)
        TRY(w_value(w, wrapper, "valid", "1", 1));
    else {
        const int n = snprintf(v, sizeof(v), "%u", (unsigned)(POLY_NEW | w->next_object++));
        TRY(w_value(w, wrapper, "id", v, (size_t)n));
    }
    return w_class(w, wrapper, "data", k, data);
}

static int w_profile_property(wr* w, uint32_t parent){
    uint32_t node;
    return w_class(w, parent, "base", C_PROFILE_PROPERTY, &node);
}

static int w_event(wr* w, uint32_t parent, const char* tag, const cape_profile_event* e){
    uint32_t data, base;
    if(e->kind == CAPE_PROFILE_DELAY_EVENT){
        TRY(w_pointer(w, parent, tag, C_DELAY_EVENT, 0, &data));
        TRY(w_class(w, data, "base", C_EVENT_BASE, &base));
        TRY(w_bool(w, data, "useRandom", e->random));
        if(e->random){
            TRY(w_u32(w, data, "delayMin", e->delay_min));
            TRY(w_u32(w, data, "delayMax", e->delay_max));
        } else
            TRY(w_u32(w, data, "delay", e->delay));
        return CAPE_OK;
    }
    if(e->kind != CAPE_PROFILE_KEY_EVENT)
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "an event of no kind the level has");
    TRY(w_pointer(w, parent, tag, C_KEY_EVENT, 0, &data));
    TRY(w_class(w, data, "base", C_EVENT_BASE, &base));
    TRY(w_keys(w, data, "keys", e->keys, e->nkeys));
    return w_enum(w, data, "sub", e_sub, e->sub);
}

static int w_action(wr* w, uint32_t parent, const cape_profile_action* a){
    static const klass kinds[] = { [CAPE_PROFILE_REMAP] = C_KEY_REMAP, [CAPE_PROFILE_MACRO] = C_MACRO, [CAPE_PROFILE_TEXT] = C_TEXT,
                                   [CAPE_PROFILE_MEDIA] = C_MEDIA };
    if((unsigned)a->kind >= COUNT(kinds))
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "an action of no kind the level has");
    uint32_t data, base, node;
    TRY(w_pointer(w, parent, "first", kinds[a->kind], 0, &data));
    TRY(w_class(w, data, "base", C_BASE_ACTION, &base));
    TRY(w_text(w, base, "name", &a->name));
    TRY(w_uuid(w, base, "id", &a->id, 1));
    TRY(w_class(w, base, "repeatOptions", C_REPEAT, &node));
    TRY(w_u32(w, node, "repeatCount", a->repeat_count));
    TRY(w_enum(w, node, "repeatMode", e_repeat, a->repeat_mode));
    TRY(w_u32(w, node, "delay", a->repeat_delay));
    TRY(w_enum(w, node, "delayMode", e_delay, a->delay_mode));
    TRY(w_u32(w, node, "randomDelayFrom", a->random_delay_from));
    TRY(w_u32(w, node, "randomDelayTo", a->random_delay_to));
    TRY(w_class(w, base, "executionHints", C_EXEC_HINTS, &node));
    TRY(w_bool(w, node, "terminateOnSecondExec", a->terminate_on_second));
    TRY(w_bool(w, node, "restartOnSecondExec", a->restart_on_second));
    TRY(w_enum(w, node, "execHint", e_hint, a->exec_hint));
    TRY(w_bool(w, node, "retainOriginalKeyOutput", a->retain_original));
    TRY(w_uuid(w, base, "actionLighting", &a->lighting, 0));
    TRY(w_text(w, base, "actionSoundPath", &a->sound_path));
    TRY(w_collection(w, base, "attachedActions", &node));
    switch(a->kind){
    case CAPE_PROFILE_REMAP:
        if(!key_ok(a->key_name.p, a->key_name.len, 1))
            return wfail(w, CAPE_PROFILE_E_CORRUPT, "keyName: not a key name");
        TRY(w_text(w, data, "keyName", &a->key_name));
        TRY(w_keys(w, data, "keyStroke", a->stroke, a->nstroke));
        TRY(w_bool(w, data, "holdingKeyEnabled", a->holding_enabled));
        TRY(w_enum(w, data, "holdingKeyType", e_hold, a->holding_type));
        TRY(w_u32(w, data, "holdingKeyOnPressInterval", a->holding_interval));
        TRY(w_text(w, data, "programPath", &a->program_path));
        TRY(w_enum(w, data, "sniperSwitchMode", e_sniper, a->sniper_mode));
        return w_u32(w, data, "remapGroup", a->remap_group);
    case CAPE_PROFILE_MACRO: {
        char name[16];
        TRY(w_enum(w, data, "terminationMethod", e_terminate, a->termination));
        TRY(w_bool(w, data, "highPriority", a->high_priority));
        TRY(w_collection(w, data, "events", &node));
        for(uint32_t i = 0; i < a->nevents; i++)
            TRY(w_event(w, node, item_name(name, i), &a->events[i]));
        return CAPE_OK;
    }
    case CAPE_PROFILE_TEXT:
        TRY(w_text(w, data, "text", &a->text));
        return w_u32(w, data, "delayBetweenCharacters", a->char_delay);
    default:
        return w_enum(w, data, "action", e_media, a->media);
    }
}

static int w_colours(wr* w, uint32_t parent, const uint32_t colours[2]){
    uint32_t node;
    TRY(w_node(w, parent, "colors", 0, NULL, 0, &node));
    TRY(w_u32(w, node, "first", colours[0]));
    return w_u32(w, node, "second", colours[1]);
}

static int w_animation(wr* w, uint32_t parent, const char* tag, const cape_profile_effect* e){
    uint32_t node;
    TRY(w_class(w, parent, tag, C_ANIMATION, &node));
    TRY(w_text(w, node, "name", &e->name));
    TRY(w_uuid(w, node, "id", &e->id, 1));
    TRY(w_u32(w, node, "duration", e->duration));
    return w_real(w, node, "brightness", e->brightness);
}

static int w_with_transitions(wr* w, uint32_t parent, const cape_profile_effect* e){
    uint32_t node, list, t;
    char name[16];
    TRY(w_class(w, parent, "base", C_TRANSITIONS, &node));
    TRY(w_animation(w, node, "base", e));
    TRY(w_collection(w, node, "transitions", &list));
    for(uint32_t i = 0; i < e->ntransitions; i++){
        TRY(w_class(w, list, item_name(name, i), C_TRANSITION, &t));
        TRY(w_real(w, t, "time", e->transitions[i].time));
        TRY(w_colour(w, t, "color", &e->transitions[i].colour));
    }
    return CAPE_OK;
}

static int w_effect(wr* w, uint32_t parent, const cape_profile_effect* e){
    if((unsigned)e->kind >= CAPE_PROFILE_EFFECTS)
        return wfail(w, CAPE_PROFILE_E_CORRUPT, "an effect of no kind the level has");
    if(!cape_profile_solid_valid(e)) return wfail(w, CAPE_PROFILE_E_CORRUPT, "Solid blocks need paired points of one colour");
    const klass k = (klass)((int)C_STATIC + (int)e->kind);
    uint32_t d;
    TRY(w_pointer(w, parent, "lighting", k, 0, &d));
    switch(k){
    case C_STATIC:
        TRY(w_with_transitions(w, d, e));
        return w_u32(w, d, "opacity", e->opacity);
    case C_SOLID:
    case C_GRADIENT:
        return w_with_transitions(w, d, e);
    case C_WAVE:
        TRY(w_with_transitions(w, d, e));
        TRY(w_real(w, d, "velocity", e->velocity));
        TRY(w_real(w, d, "tailLength", e->tail_length));
        TRY(w_bool(w, d, "isDoublesided", e->double_sided));
        return w_real(w, d, "angle", e->angle);
    case C_RIPPLE:
        TRY(w_with_transitions(w, d, e));
        TRY(w_real(w, d, "velocity", e->velocity));
        TRY(w_real(w, d, "tailLength", e->tail_length));
        TRY(w_bool(w, d, "autoDuration", e->auto_duration));
        TRY(w_enum(w, d, "waveSpread", e_spread, e->wave_spread));
        return w_colours(w, d, e->colours);
    case C_COLOR_PULSE:
    case C_COLOR_SHIFT:
        TRY(w_animation(w, d, "base", e));
        TRY(w_colours(w, d, e->colours));
        TRY(w_bool(w, d, "random", e->random));
        return w_u32(w, d, "speed", e->speed);
    case C_COLOR_WAVE:
        TRY(w_animation(w, d, "value0", e));
        TRY(w_u32(w, d, "speed", e->speed));
        TRY(w_bool(w, d, "random", e->random));
        TRY(w_colours(w, d, e->colours));
        return w_u32(w, d, "direction", e->direction);
    case C_RAIN:
        TRY(w_animation(w, d, "base", e));
        TRY(w_colours(w, d, e->colours));
        TRY(w_bool(w, d, "random", e->random));
        TRY(w_u32(w, d, "opacity", e->opacity));
        TRY(w_u32(w, d, "speed", e->speed));
        return w_u32(w, d, "direction", e->direction);
    case C_RAINBOW_WAVE:
        TRY(w_animation(w, d, "base", e));
        TRY(w_u32(w, d, "speed", e->speed));
        return w_u32(w, d, "direction", e->direction);
    case C_SPIRAL_RAINBOW:
        TRY(w_animation(w, d, "base", e));
        TRY(w_bool(w, d, "clockwise", e->clockwise));
        return w_u32(w, d, "speed", e->speed);
    case C_TYPE:
        TRY(w_animation(w, d, "base", e));
        TRY(w_colours(w, d, e->colours));
        TRY(w_bool(w, d, "random", e->random));
        TRY(w_u32(w, d, "opacity", e->opacity));
        TRY(w_u32(w, d, "speed", e->speed));
        TRY(w_u32(w, d, "duration", e->type_duration));
        return w_u32(w, d, "typeLightingBehavior", e->type_behaviour);
    default:   // C_VISOR
        TRY(w_animation(w, d, "base", e));
        TRY(w_colours(w, d, e->colours));
        TRY(w_bool(w, d, "random", e->random));
        TRY(w_u32(w, d, "speed", e->speed));
        return w_u32(w, d, "direction", e->direction);
    }
}

static int w_layer(wr* w, uint32_t parent, const char* tag, const cape_profile_layer* l){
    const char* error = cape_profile_layer_error(l);
    if(error) return wfail(w, CAPE_PROFILE_E_CORRUPT, "%s", error);
    uint32_t data, base, node;
    TRY(w_pointer(w, parent, tag, C_LAYER, 0, &data));
    TRY(w_class(w, data, "base", C_LIGHTING_LAYER, &base));
    TRY(w_uuid(w, base, "id", &l->id, 1));
    TRY(w_text(w, base, "name", &l->name));
    TRY(w_keys(w, base, "keys", l->keys, l->nkeys));
    TRY(w_bool(w, base, "enabled", l->enabled));
    TRY(w_class(w, base, "executionHints", C_LIGHTING_HINTS, &node));
    TRY(w_bool(w, node, "startOnKeyPress", l->start_on_key_press));
    TRY(w_bool(w, node, "startWithProfile", l->start_with_profile));
    TRY(w_bool(w, node, "stopOnKeyPress", l->stop_on_key_press));
    TRY(w_bool(w, node, "stopOnKeyRelease", l->stop_on_key_release));
    TRY(w_i32(w, node, "stopAfterTimes", l->stop_after_times));
    TRY(w_enum(w, node, "playOption", e_play, l->play_option));
    TRY(w_enum(w, base, "option", e_option, l->option));
    return w_effect(w, data, &l->effect);
}

static int w_profile(wr* w, const cape_profile_profile* p){
    uint32_t root, data, base, node, list;
    char name[16];
    TRY(w_class(w, 0, "profile", C_BACKUP_PROFILE, &root));
    TRY(w_bool(w, root, "isAdvancedMode", p->advanced));
    TRY(w_pointer(w, root, "LightingsProperty", C_LIGHTINGS, 1, &data));
    TRY(w_class(w, data, "base", C_CHANNEL_BASE, &base));
    TRY(w_profile_property(w, base));
    TRY(w_collection(w, base, "layers", &list));
    for(uint32_t i = 0; i < p->nlayers; i++)
        TRY(w_layer(w, list, item_name(name, i), &p->layers[i]));
    TRY(w_pointer(w, root, "BrightnessIndicatorProperty", C_BRIGHTNESS_IND, 1, &data));
    TRY(w_profile_property(w, data));
    TRY(w_colour(w, data, "indicatorColor", &p->brightness));
    TRY(w_pointer(w, root, "WinlockIndicatorProperty", C_WINLOCK_IND, 1, &data));
    TRY(w_profile_property(w, data));
    TRY(w_optcolour(w, data, "enabledIndicatorColor", &p->winlock_on));
    TRY(w_optcolour(w, data, "disabledIndicatorColor", &p->winlock_off));
    TRY(w_pointer(w, root, "ProfileIndicatorProperty", C_PROFILE_IND, 1, &data));
    TRY(w_profile_property(w, data));
    TRY(w_colour(w, data, "m1Color", &p->m1));
    TRY(w_colour(w, data, "m2Color", &p->m2));
    TRY(w_colour(w, data, "m3Color", &p->m3));
    TRY(w_optcolour(w, data, "profileIndicatorColor", &p->profile));
    TRY(w_pointer(w, root, "WinlockProperty", C_WINLOCK, 1, &data));
    TRY(w_profile_property(w, data));
    TRY(w_bool(w, data, "disableAltTab", p->disable_alt_tab));
    TRY(w_bool(w, data, "disableAltF4", p->disable_alt_f4));
    TRY(w_bool(w, data, "disableWinKey", p->disable_win));
    TRY(w_bool(w, data, "disableShiftTab", p->disable_shift_tab));
    TRY(w_bool(w, data, "disableEncryption", p->disable_encryption));
    TRY(w_bool(w, data, "disableTouchpad", p->disable_touchpad));
    TRY(w_bool(w, data, "swapFKeyFunctions", p->swap_fkeys));
    TRY(w_pointer(w, root, "SideLightingProperty", C_SIDE_LIGHTING, 1, &data));
    TRY(w_profile_property(w, data));
    TRY(w_bool(w, data, "enabled", p->side_lighting));
    TRY(w_pointer(w, root, "ActionsProperty", C_HW_ACTIONS, 1, &data));
    TRY(w_class(w, data, "base", C_ACTIONS_BASE, &base));
    TRY(w_profile_property(w, base));
    TRY(w_collection(w, base, "actions", &list));
    for(uint32_t i = 0; i < p->nbindings; i++){
        const cape_profile_binding* b = &p->bindings[i];
        uint32_t pair, assoc;
        TRY(w_node(w, list, item_name(name, i), 0, NULL, 0, &pair));
        TRY(w_action(w, pair, &b->action));
        TRY(w_class(w, pair, "second", C_KEY_ASSOCIATION, &assoc));
        if(!key_ok(b->key.p, b->key.len, 1))
            return wfail(w, CAPE_PROFILE_E_CORRUPT, "binding %u: not a key name", (unsigned)i);
        TRY(w_text(w, assoc, "key", &b->key));
        TRY(w_enum(w, assoc, "layer", e_layer, b->layer));
        TRY(w_enum(w, assoc, "event", e_event, b->event));
        TRY(w_u32(w, assoc, "distance", b->distance));
    }
    TRY(w_pointer(w, base, "presetsInfo", C_PRESETS, 0, &node));
    TRY(w_uuid(w, node, "activePresetId", &p->active_preset, 0));
    TRY(w_bool(w, node, "isActive", p->preset_active));
    return w_u32(w, data, "hwActionsSupportedTypes", p->hw_actions_types);
}

int cape_profile_build(const cape_profile_profile* p, cape_xml_tree* t, char* why){
    if(!p || !t)
        return CAPE_E_ARG;
    cape_xml_init(t);
    if(why)
        why[0] = 0;
    wr w;
    memset(&w, 0, sizeof(w));
    w.t = t;
    w.next_object = 1;
    w.why = why;
    int rc = w_node(&w, CAPE_XML_NONE, "cereal", 0, NULL, 0, NULL);
    if(rc == CAPE_OK)
        rc = w_profile(&w, p);
    free(w.ids.v);
    if(rc != CAPE_OK)
        cape_xml_free(t);
    return rc;
}

// The document of a model, in a new buffer (to free)
static int emit(const cape_profile_profile* p, uint8_t** out, size_t* len){
    cape_xml_tree t;
    int rc = cape_profile_build(p, &t, NULL);
    if(rc != CAPE_OK)
        return rc;
    const int n = cape_xml_emit(&t, NULL, 0);
    if(n < 0){
        cape_xml_free(&t);
        return n;
    }
    *out = malloc((size_t)n ? (size_t)n : 1);
    if(!*out){
        cape_xml_free(&t);
        return CAPE_PROFILE_E_MEMORY;
    }
    rc = cape_xml_emit(&t, *out, (size_t)n);
    cape_xml_free(&t);
    if(rc < 0){
        free(*out);
        return rc;
    }
    *len = (size_t)n;
    return CAPE_OK;
}

int cape_profile_equal(const cape_profile_profile* a, const cape_profile_profile* b){
    if(!a || !b)
        return CAPE_E_ARG;
    uint8_t *x = NULL, *y = NULL;
    size_t nx = 0, ny = 0;
    int rc = emit(a, &x, &nx);
    if(rc == CAPE_OK)
        rc = emit(b, &y, &ny);
    if(rc == CAPE_OK)
        rc = nx == ny && !memcmp(x, y, nx);
    free(x);
    free(y);
    return rc;
}
