#include "cape_light.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int cape_light_cell(int led){
    if(led < 0 || led > CAPE_LED_LAST)
        return -1;
    if(led >= 144)
        return led;
    int c = led / 12, r = led % 12;
    if(r < 8)
        return 8 * c + r;
    return 8 * (12 + c % 6) + 4 * (c / 6) + (r - 8);
}

// The .k of the layer over all keys of iCUE, cell by cell: rows of the main block (cells 0 to 95, the row is the cell % 8), then
// the media keys, the numpad, the G keys, and the top bar in its own order
static const uint8_t canon[CAPE_CANON_COUNT] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88,
    1, 9, 17, 25, 33, 41, 49, 57, 65, 73, 81, 89,
    2, 10, 18, 26, 34, 42, 50, 58, 66, 74, 82, 90,
    3, 11, 19, 27, 35, 43, 51, 59, 67, 75, 83, 91,
    4, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92,
    5, 13, 21, 37, 61, 69, 77,
    6, 14, 22, 30, 38, 46, 54, 62, 78, 86,
    7, 23, 31, 39, 47, 55, 63, 71, 79, 87, 95,
    104, 112, 120, 128, 136,
    100, 108, 116, 124, 132, 140,
    97, 105, 113, 129, 137,
    101, 109, 117, 125, 133, 141,
    98, 106, 114, 122, 130, 138,
    144, 145, 146, 158, 160, 147, 148, 149, 150, 151, 152, 153, 154, 155, 159, 162, 161, 156, 157,
};

int cape_light_canon_rank(int cell){
    if(cell < 0 || cell > 0xff)
        return -1;
    for(int i = 0; i < CAPE_CANON_COUNT; i++){
        if(canon[i] == cell)
            return i;
    }
    return -1;
}

int cape_light_canon_cell(int rank){
    if(rank < 0 || rank >= CAPE_CANON_COUNT)
        return -1;
    return canon[rank];
}

int cape_light_led(int cell){
    if(cape_light_canon_rank(cell) < 0)
        return -1;
    if(cell >= 144)
        return cell;
    if(cell < 96)
        return 12 * (cell / 8) + cell % 8;
    // Cells 96 to 143: eight to a hardware column (12 to 17), the daemon's columns 0 to 5 in its rows 8 to 11 and its columns
    // 6 to 11 in the second half of the column
    const int col = cell / 8 - 12, within = cell % 8;
    return 12 * (col + 6 * (within / 4)) + 8 + within % 4;
}

// ---------------------------------------------------------------------------------------------------------
// From colours to layers

// The colours a picture has on the cells of the table, in the order of their first cell
typedef struct {
    uint8_t rgb[CAPE_CANON_COUNT][3];
    unsigned n;                        // distinct colours other than black
    int of[CAPE_CANON_COUNT];          // for each position of the canonical order: the index of its colour, -1 for black
    unsigned lit;
    unsigned ignored;
} picture;

static int led_black(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, int led){
    return led < 0 || (size_t)led >= n || (!r[led] && !g[led] && !b[led]);
}

static void scan_picture(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, picture* p){
    memset(p, 0, sizeof(*p));
    for(int rank = 0; rank < CAPE_CANON_COUNT; rank++){
        const int led = cape_light_led(cape_light_canon_cell(rank));
        p->of[rank] = -1;
        if(led_black(r, g, b, n, led))
            continue;
        unsigned k = 0;
        while(k < p->n && (p->rgb[k][0] != r[led] || p->rgb[k][1] != g[led] || p->rgb[k][2] != b[led]))
            k++;
        if(k == p->n){
            p->rgb[k][0] = r[led];
            p->rgb[k][1] = g[led];
            p->rgb[k][2] = b[led];
            p->n++;
        }
        p->of[rank] = (int)k;
        p->lit++;
    }
    // The LEDs that have colour and no place in a file
    for(int led = 0; led <= CAPE_LED_LAST && (size_t)led < n; led++){
        if(led == CAPE_LED_PROFILE || led == CAPE_LED_BRIGHTNESS || led == CAPE_LED_WINLOCK || led_black(r, g, b, n, led))
            continue;
        if(cape_light_canon_rank(cape_light_cell(led)) < 0)
            p->ignored++;
    }
}

int cape_light_pack_ordered(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, const uint8_t (*order)[3], size_t norder,
                            cape_light_pack_result* out){
    if(!r || !g || !b || !out || (!order && norder))
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    picture p;
    scan_picture(r, g, b, n, &p);
    out->colours = p.n;
    out->lit = p.lit;
    out->ignored = p.ignored;
    if(p.n > CAPE_LAYERS_MAX)
        return CAPE_LIGHT_E_COLOURS;

    // The picture's colours in the order the layers are written: the first appearance, or the order that was given
    unsigned idx[CAPE_LAYERS_MAX];
    unsigned taken[CAPE_LAYERS_MAX] = { 0 };
    unsigned count = 0;
    if(order){
        for(size_t o = 0; o < norder; o++){
            for(unsigned k = 0; k < p.n; k++){
                if(!taken[k] && !memcmp(p.rgb[k], order[o], 3)){
                    taken[k] = 1;
                    idx[count++] = k;
                    break;
                }
            }
        }
        if(count != p.n)
            return CAPE_LIGHT_E_ORDER;
    } else {
        for(unsigned k = 0; k < p.n; k++)
            idx[count++] = k;
    }
    for(unsigned l = 0; l < count; l++){
        cape_light_layer* ly = &out->layer[l];
        memcpy(ly->rgb, p.rgb[idx[l]], 3);
        for(int rank = 0; rank < CAPE_CANON_COUNT; rank++){
            if(p.of[rank] == (int)idx[l])
                ly->cells[ly->n++] = (uint8_t)cape_light_canon_cell(rank);
        }
    }
    out->layers = count;
    return CAPE_OK;
}

int cape_light_pack(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, cape_light_pack_result* out){
    return cape_light_pack_ordered(r, g, b, n, NULL, 0, out);
}

unsigned cape_light_buttons_changed(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n,
                                    const uint8_t ind[CAPE_IND_COUNT][3]){
    if(!ind || !r || !g || !b)
        return 0;
    static const struct { int led; int which; unsigned bit; } buttons[] = {
        { CAPE_LED_PROFILE, CAPE_IND_PROFILE, CAPE_BTN_PROFILE }, { CAPE_LED_BRIGHTNESS, CAPE_IND_BRIGHTNESS, CAPE_BTN_BRIGHTNESS },
        { CAPE_LED_WINLOCK, CAPE_IND_LOCK_OFF, CAPE_BTN_WINLOCK },
    };
    unsigned mask = 0;
    for(size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++){
        const int led = buttons[i].led;
        const uint8_t have[3] = { (size_t)led < n ? r[led] : 0, (size_t)led < n ? g[led] : 0, (size_t)led < n ? b[led] : 0 };
        if(memcmp(have, ind[buttons[i].which], 3) != 0)
            mask |= buttons[i].bit;
    }
    return mask;
}

int cape_light_files_build(const cape_light_pack_result* pack, cape_light_file* files, size_t cap, size_t* n){
    if(!pack || !files || !n)
        return CAPE_E_ARG;
    *n = 0;
    if(pack->layers > CAPE_LAYERS_MAX)
        return CAPE_E_RANGE;
    if(cap < 1 + 3 * (size_t)pack->layers)
        return CAPE_E_CAP;
    size_t k = 0;
    for(unsigned l = 0; l < pack->layers; l++){
        const cape_light_layer* ly = &pack->layer[l];
        if(ly->n == 0 || ly->n > CAPE_CANON_COUNT)
            return CAPE_E_RANGE;
        char name[10];
        cape_light_desc d;
        cape_light_desc_static(&d);
        if(cape_light_filename(l, 'd', name) != CAPE_OK)
            return CAPE_E_RANGE;
        snprintf(files[k].name, sizeof(files[k].name), "%s", name);
        int len = cape_light_desc_build(&d, files[k].data, sizeof(files[k].data));
        if(len < 0)
            return len;
        files[k++].len = (size_t)len;

        cape_light_filename(l, 'k', name);
        snprintf(files[k].name, sizeof(files[k].name), "%s", name);
        len = cape_keylist_build(ly->cells, ly->n, files[k].data, sizeof(files[k].data));
        if(len < 0)
            return len;
        files[k++].len = (size_t)len;

        cape_rgba colours[CAPE_COLORS_COUNT];
        memset(colours, 0, sizeof(colours));
        for(unsigned i = 0; i < ly->n; i++){
            colours[i].r = ly->rgb[0];
            colours[i].g = ly->rgb[1];
            colours[i].b = ly->rgb[2];
            colours[i].a = 0xff;
        }
        cape_light_filename(l, 'r', name);
        snprintf(files[k].name, sizeof(files[k].name), "%s", name);
        len = cape_colors_build(colours, files[k].data, sizeof(files[k].data));
        if(len < 0)
            return len;
        files[k++].len = (size_t)len;
    }
    snprintf(files[k].name, sizeof(files[k].name), "%s", CAPE_FILE_LAYERCOUNT);
    int len = cape_layercount_build(pack->layers, files[k].data, sizeof(files[k].data));
    if(len < 0)
        return len;
    files[k++].len = (size_t)len;
    *n = k;
    return CAPE_OK;
}

// Formats a reason into a caller's buffer, if it has one
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void put_why(char* why, size_t cap, const char* fmt, ...){
    if(!why || !cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

cape_layer_kind cape_light_layer_kind(const uint8_t* d, size_t len, char* why, size_t cap){
    if(why && cap)
        why[0] = '\0';
    if(!d || len == 0){
        put_why(why, cap, "the descriptor is empty");
        return CAPE_LAYER_UNKNOWN;
    }
    // Which sizes and types go together is the codec's knowledge (cape_light_desc_family); what a static layer may
    // hold in the rest of its descriptor is ours
    const cape_ldesc_family family = cape_light_desc_family(d, len);
    if(family == CAPE_LDESC_PREDEFINED)
        return CAPE_LAYER_PREDEFINED;
    if(family == CAPE_LDESC_CUSTOM)
        return CAPE_LAYER_CUSTOM;
    if(family == CAPE_LDESC_STATIC){
        cape_light_desc ref;
        uint8_t want[CAPE_LIGHT_DESC_STATIC];
        cape_light_desc_static(&ref);
        if(cape_light_desc_build(&ref, want, sizeof(want)) != CAPE_LIGHT_DESC_STATIC){
            put_why(why, cap, "the reference descriptor of a static layer cannot be built");
            return CAPE_LAYER_UNKNOWN;
        }
        char where[64] = "";
        size_t used = 0;
        unsigned differing = 0;
        for(size_t i = 0; i < len; i++){
            if(d[i] == want[i])
                continue;
            if(differing < 6 && used < sizeof(where))
                used += (size_t)snprintf(where + used, sizeof(where) - used, differing ? ", %u" : "%u", (unsigned)i);
            differing++;
        }
        if(!differing)
            return CAPE_LAYER_STATIC;
        put_why(why, cap, "the static layer descriptor differs from iCUE's at byte%s %s%s", differing > 1 ? "s" : "", where,
                differing > 6 ? ", ..." : "");
        return CAPE_LAYER_UNKNOWN;
    }
    put_why(why, cap, "a descriptor of %u bytes with effect type %u is not one iCUE writes", (unsigned)len, (unsigned)d[0]);
    return CAPE_LAYER_UNKNOWN;
}

void cape_light_image_clear(cape_light_image* img){
    memset(img, 0, sizeof(*img));
}

unsigned cape_light_covered(const cape_light_image* img){
    unsigned n = 0;
    for(int c = 0; c < CAPE_CELLS; c++)
        n += img->covered[c] != 0;
    return n;
}

unsigned cape_light_unmapped(const cape_light_image* img){
    unsigned n = 0;
    for(int c = CAPE_LED_LAST + 1; c < CAPE_CELLS; c++)
        n += img->covered[c] != 0;
    return n;
}

const char* cape_light_strerror(int err){
    switch(err){
    case CAPE_LIGHT_E_MIXED:
        return "the colours of the layer differ from key to key";
    case CAPE_LIGHT_E_MANY:
        return "the layer lists more keys than it has colours";
    case CAPE_LIGHT_E_COLOURS:
        return "more colours than a profile has layers";
    case CAPE_LIGHT_E_ORDER:
        return "a colour of the picture is not in the order given";
    default:
        return cape_strerror(err);
    }
}

static uint8_t blend(uint8_t src, uint8_t dst, unsigned alpha){
    return (uint8_t)((src * alpha + dst * (255u - alpha) + 127u) / 255u);
}

int cape_light_paint(cape_light_image* img, const uint8_t* k, size_t klen, const uint8_t* r, size_t rlen, unsigned* dups){
    if(dups)
        *dups = 0;
    if(!img || !k || !r)
        return CAPE_E_ARG;
    // Everything is checked before the image is touched
    uint8_t cells[CAPE_COLORS_COUNT];
    int n = cape_keylist_parse(k, klen, cells, sizeof(cells));
    if(n == CAPE_E_CAP)
        return CAPE_LIGHT_E_MANY;
    if(n < 0)
        return n;
    cape_rgba col[CAPE_COLORS_COUNT];
    int rc = cape_colors_parse(r, rlen, col);
    if(rc != CAPE_OK)
        return rc;
    for(int i = 1; i < n; i++){
        if(col[i].r != col[0].r || col[i].g != col[0].g || col[i].b != col[0].b || col[i].a != col[0].a)
            return CAPE_LIGHT_E_MIXED;
    }
    if(n == 0)
        return CAPE_OK;

    uint8_t seen[CAPE_CELLS];
    memset(seen, 0, sizeof(seen));
    unsigned twice = 0;
    const unsigned alpha = col[0].a;
    for(int i = 0; i < n; i++){
        const uint8_t cell = cells[i];
        if(seen[cell]){
            twice++;
            continue;
        }
        seen[cell] = 1;
        img->rgb[cell][0] = blend(col[0].r, img->rgb[cell][0], alpha);
        img->rgb[cell][1] = blend(col[0].g, img->rgb[cell][1], alpha);
        img->rgb[cell][2] = blend(col[0].b, img->rgb[cell][2], alpha);
        img->covered[cell] = 1;
    }
    if(dups)
        *dups = twice;
    return CAPE_OK;
}

void cape_light_indicators(const cape_info* info, uint8_t out[CAPE_IND_COUNT][3]){
    for(int t = 0; t < CAPE_IND_COUNT; t++)
        memcpy(out[t], info->tail + 3 * t, 3);
}

void cape_light_leds(const cape_light_image* img, const uint8_t ind[CAPE_IND_COUNT][3], size_t n, uint8_t* r, uint8_t* g, uint8_t* b){
    if(!img || !r || !g || !b)
        return;
    for(size_t led = 0; led < n && led <= CAPE_LED_LAST; led++){
        const int cell = cape_light_cell((int)led);
        if(cell < 0)
            continue;
        r[led] = img->rgb[cell][0];
        g[led] = img->rgb[cell][1];
        b[led] = img->rgb[cell][2];
    }
    if(!ind)
        return;
    static const struct { size_t led; int ind; } buttons[] = {
        { CAPE_LED_PROFILE, CAPE_IND_PROFILE },
        { CAPE_LED_BRIGHTNESS, CAPE_IND_BRIGHTNESS },
        { CAPE_LED_WINLOCK, CAPE_IND_LOCK_OFF },
    };
    for(size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++){
        if(buttons[i].led >= n)
            continue;
        r[buttons[i].led] = ind[buttons[i].ind][0];
        g[buttons[i].led] = ind[buttons[i].ind][1];
        b[buttons[i].led] = ind[buttons[i].ind][2];
    }
}

void cape_light_slot_init(cape_light_slot* s){
    memset(s, 0, sizeof(*s));
}

const char* cape_light_state_name(cape_light_state st){
    switch(st){
    case CAPE_LIGHT_UNREAD:
        return "unread";
    case CAPE_LIGHT_STATIC:
        return "static";
    case CAPE_LIGHT_EMPTY:
        return "empty";
    case CAPE_LIGHT_EFFECTS:
        return "effects";
    case CAPE_LIGHT_UNKNOWN:
        return "unknown";
    }
    return "?";
}

void cape_light_slot_unknown(cape_light_slot* s, const char* why){
    s->state = CAPE_LIGHT_UNKNOWN;
    snprintf(s->why, sizeof(s->why), "%s", why ? why : "");
}

int cape_light_slot_count(cape_light_slot* s, uint32_t n){
    s->layers = (unsigned)n;
    if(n == 0){
        s->state = CAPE_LIGHT_EMPTY;
        snprintf(s->why, sizeof(s->why), "no layers");
        return 0;
    }
    if(n > CAPE_LAYERS_MAX){
        char why[CAPE_LIGHT_WHY];
        snprintf(why, sizeof(why), "%u layers, iCUE writes at most %u", (unsigned)n, (unsigned)CAPE_LAYERS_MAX);
        cape_light_slot_unknown(s, why);
        return 0;
    }
    return 1;
}

int cape_light_slot_kinds(cape_light_slot* s, const cape_layer_kind* kinds, unsigned n){
    s->n_static = s->n_predefined = s->n_custom = 0;
    if(!kinds || n == 0 || n > CAPE_LAYERS_MAX){
        cape_light_slot_unknown(s, "the number of layers is not one that iCUE writes");
        return 0;
    }
    for(unsigned i = 0; i < n; i++){
        switch(kinds[i]){
        case CAPE_LAYER_STATIC:
            s->n_static++;
            break;
        case CAPE_LAYER_PREDEFINED:
            s->n_predefined++;
            break;
        case CAPE_LAYER_CUSTOM:
            s->n_custom++;
            break;
        default: {
            char why[CAPE_LIGHT_WHY];
            snprintf(why, sizeof(why), "layer %u is of a kind that is not understood", i);
            cape_light_slot_unknown(s, why);
            return 0;
        }
        }
    }
    if(s->n_predefined){
        if(s->n_predefined == 1 && n == 1){
            s->state = CAPE_LIGHT_EFFECTS;
            snprintf(s->why, sizeof(s->why), "a predefined effect");
        } else {
            cape_light_slot_unknown(s, "a predefined effect together with other layers (iCUE only writes it alone)");
        }
        return 0;
    }
    if(s->n_custom){
        s->state = CAPE_LIGHT_EFFECTS;
        if(s->n_static)
            snprintf(s->why, sizeof(s->why), "%u custom effect layer%s and %u static", s->n_custom, s->n_custom > 1 ? "s" : "", s->n_static);
        else
            snprintf(s->why, sizeof(s->why), "%u custom effect layer%s", s->n_custom, s->n_custom > 1 ? "s" : "");
        return 0;
    }
    return 1;
}

// Room for a file of the lighting: the largest is lght_NN.r, 1024 bytes; anything bigger is not one of ours
#define LIGHT_BUF  2048

static int light_read(cape_light_slot* s, cape_light_source src, void* ctx, int all);

int cape_light_read(cape_light_slot* s, cape_light_source src, void* ctx){
    return light_read(s, src, ctx, 0);
}

int cape_light_read_all(cape_light_slot* s, cape_light_source src, void* ctx){
    return light_read(s, src, ctx, 1);
}

// The files of the layers of a slot of effects that the first pass did not read: .k of the custom ones, .k and .r of the static ones
static int read_rest(cape_light_slot* s, cape_light_source src, void* ctx){
    uint8_t buf[LIGHT_BUF];
    for(unsigned i = 0; i < s->layers; i++){
        const char* exts = s->kind[i] == CAPE_LAYER_STATIC ? "kr" : s->kind[i] == CAPE_LAYER_CUSTOM ? "k" : "";
        for(const char* e = exts; *e; e++){
            char name[10];
            size_t len = 0;
            cape_light_filename(i, *e, name);
            const int rc = src(ctx, name, buf, sizeof(buf), &len);
            if(rc < 0)
                return rc;
            if(rc > 0){
                snprintf(s->missing, sizeof(s->missing), "%s", name);
                return CAPE_OK;
            }
        }
    }
    s->complete = 1;
    return CAPE_OK;
}

static int light_read(cape_light_slot* s, cape_light_source src, void* ctx, int all){
    if(!s || !src)
        return CAPE_E_ARG;
    // A slot is read from scratch: whatever it held is forgotten, but for the indicator colours the caller set
    uint8_t ind[CAPE_IND_COUNT][3];
    memcpy(ind, s->ind, sizeof(ind));
    const int ind_ok = s->ind_ok;
    cape_light_slot_init(s);
    memcpy(s->ind, ind, sizeof(ind));
    s->ind_ok = ind_ok;

    uint8_t buf[LIGHT_BUF];
    size_t len = 0;
    char name[10];
    char why[CAPE_LIGHT_WHY];

    int rc = src(ctx, CAPE_FILE_LAYERCOUNT, buf, sizeof(buf), &len);
    if(rc < 0)
        return rc;
    if(rc > 0){
        cape_light_slot_unknown(s, CAPE_FILE_LAYERCOUNT " is missing or too large");
        return CAPE_OK;
    }
    uint32_t n = 0;
    if(cape_layercount_parse(buf, len, &n) != CAPE_OK){
        cape_light_slot_unknown(s, CAPE_FILE_LAYERCOUNT " is not valid");
        return CAPE_OK;
    }
    if(!cape_light_slot_count(s, n)){
        s->complete = s->state == CAPE_LIGHT_EMPTY;
        return CAPE_OK;
    }

    // First pass: what every layer is
    cape_layer_kind kinds[CAPE_LAYERS_MAX];
    int have_predefined = 0;
    for(unsigned i = 0; i < n; i++){
        cape_light_filename(i, 'd', name);
        rc = src(ctx, name, buf, sizeof(buf), &len);
        if(rc < 0)
            return rc;
        if(rc > 0){
            snprintf(why, sizeof(why), "%s is missing or too large", name);
            cape_light_slot_unknown(s, why);
            return CAPE_OK;
        }
        char reason[CAPE_LIGHT_WHY - 24];    // room for "layer 4294967295: " in front of it in why
        kinds[i] = cape_light_layer_kind(buf, len, reason, sizeof(reason));
        if(kinds[i] == CAPE_LAYER_UNKNOWN){
            snprintf(why, sizeof(why), "layer %u: %s", i, reason);
            cape_light_slot_unknown(s, why);
            return CAPE_OK;
        }
        if(kinds[i] == CAPE_LAYER_PREDEFINED && !have_predefined){
            memcpy(s->predefined, buf, CAPE_LIGHT_DESC_PREDEFINED);
            have_predefined = 1;
        }
    }
    memcpy(s->kind, kinds, n * sizeof(kinds[0]));
    if(!cape_light_slot_kinds(s, kinds, n))
        return all && s->state == CAPE_LIGHT_EFFECTS ? read_rest(s, src, ctx) : CAPE_OK;

    // Second pass: all static, so the layers are painted in file order
    uint8_t kbuf[LIGHT_BUF];
    for(unsigned i = 0; i < n; i++){
        size_t klen = 0;
        cape_light_filename(i, 'k', name);
        rc = src(ctx, name, kbuf, sizeof(kbuf), &klen);
        if(rc == 0){
            cape_light_filename(i, 'r', name);
            rc = src(ctx, name, buf, sizeof(buf), &len);
        }
        if(rc < 0)
            return rc;
        if(rc > 0){
            snprintf(why, sizeof(why), "%s is missing or too large", name);
            cape_light_slot_unknown(s, why);
            return CAPE_OK;
        }
        unsigned twice = 0;
        rc = cape_light_paint(&s->image, kbuf, klen, buf, len, &twice);
        if(rc != CAPE_OK){
            snprintf(why, sizeof(why), "layer %u: %s", i, cape_light_strerror(rc));
            cape_light_slot_unknown(s, why);
            return CAPE_OK;
        }
        s->dup_cells += twice;
    }
    s->state = CAPE_LIGHT_STATIC;
    s->complete = 1;
    return CAPE_OK;
}
