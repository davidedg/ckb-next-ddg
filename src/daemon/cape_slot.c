#include "cape_slot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------
// A set of files

void cape_slot_files_free(cape_slot_files* f){
    if(!f)
        return;
    for(size_t i = 0; i < f->n; i++){
        free(f->file[i].data);
        f->file[i].data = NULL;
    }
    f->n = 0;
}

const cape_slot_file* cape_slot_files_find(const cape_slot_files* f, const char* name){
    if(!f || !name)
        return NULL;
    for(size_t i = 0; i < f->n; i++){
        if(!strcmp(f->file[i].name, name))
            return &f->file[i];
    }
    return NULL;
}

int cape_slot_files_add(cape_slot_files* f, const char* name, const uint8_t* data, size_t len){
    if(!f || !name || (!data && len))
        return CAPE_E_ARG;
    const size_t nl = strlen(name);
    if(nl == 0 || nl >= CAPE_SLOT_NAME || cape_slot_files_find(f, name))
        return CAPE_E_RANGE;
    if(f->n >= CAPE_SLOT_FILES_MAX)
        return CAPE_E_CAP;
    uint8_t* copy = malloc(len ? len : 1);
    if(!copy)
        return CAPE_E_CAP;
    if(len)
        memcpy(copy, data, len);
    snprintf(f->file[f->n].name, sizeof(f->file[f->n].name), "%s", name);
    f->file[f->n].data = copy;
    f->file[f->n].len = len;
    f->n++;
    return CAPE_OK;
}

const char* cape_slot_verdict_name(cape_slot_verdict v){
    switch(v){
    case CAPE_SLOT_REFUSED:
        return "refused";
    case CAPE_SLOT_SAME:
        return "same";
    case CAPE_SLOT_WRITE:
        return "write";
    case CAPE_SLOT_NEW:
        return "new";
    }
    return "?";
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void put_reason(char* out, size_t cap, const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------------------------------------
// Reading a slot

void cape_slot_image_free(cape_slot_image* img){
    if(!img)
        return;
    cape_slot_files_free(&img->files);
    cape_slot_files_free(&img->light_files);
    cape_binding_free(&img->bind);
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void refuse(cape_slot_image* img, const char* fmt, ...){
    if(img->refused)
        return;
    img->refused = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->reason, sizeof(img->reason), fmt, ap);
    va_end(ap);
}

// Where the files of a slot come from: the keyboard (a session) or memory (the files of a save that were written and read back).
// 0: the file was read; 1: it does not exist; 2: it is larger than cap; negative: the reading failed (a session that failed)
typedef int (*slot_source)(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len);

typedef struct {
    cape_session* s;
    unsigned slot;
} session_ctx;

// How many files a slot has, once the DAT says how many macro files: PROFILE.I, .DAT, .MAP, the macro files and the lighting (a
// guess: a count and five layers of three files). Only a session keeps count of that (the progress of a save's read)
static void files_known(slot_source get, void* ctx, unsigned nmacros);

static int session_get(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len){
    const session_ctx* c = ctx;
    int rc = cape_read_file(c->s, c->slot, name, buf, cap, len);
    if(rc == CAPE_OK)
        return 0;
    if(rc == CAPE_E_NOTFOUND)
        return 1;
    if(rc == CAPE_E_CAP)
        return 2;
    return rc;
}

static void files_known(slot_source get, void* ctx, unsigned nmacros){
    if(get == session_get)
        cape_session_progress_expect(((session_ctx*)ctx)->s, 3 + nmacros + 1 + 3 * CAPE_LAYERS_MAX);
}

static int memory_get(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len){
    const cape_slot_file* f = cape_slot_files_find(ctx, name);
    if(!f)
        return 1;
    if(f->len > cap)
        return 2;
    memcpy(buf, f->data, f->len);
    *len = f->len;
    return 0;
}

// The files of the lighting, for cape_light_read_all(): one that is missing or too large is not one of ours (1), a failure of the
// reading ends it. Every file read is kept, to be written back as it is by a save that does not change the lighting.
typedef struct {
    slot_source get;
    void* ctx;
    cape_slot_files* record;
} light_ctx;

static int light_get(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len){
    const light_ctx* c = ctx;
    const int st = c->get(c->ctx, name, buf, cap, len);
    if(st == 0 && cape_slot_files_add(c->record, name, buf, *len) != CAPE_OK)
        return CAPE_E_CAP;
    return st > 0 ? 1 : st;
}

// Why a file could not be read: st is 1 (it does not exist) or 2 (it is larger than the cap)
static void file_why(char* out, size_t cap, const char* what, int st){
    put_reason(out, cap, st == 1 ? "%s does not exist" : "%s is larger than expected", what);
}

// Reads the files of the bindings and imports them: whatever cannot be read or kept is a state of the model (BROKEN), not a
// refusal of the slot, and not an end of the reading. Returns CAPE_OK or the failure of the reading.
static int read_bindings(slot_source get, void* ctx, cape_slot_image* img, uint8_t* buf){
    const size_t cap = CAPE_MAX_FILE_SIZE;
    size_t len = 0;
    char why[CAPE_BIND_WHY] = "";
    int dat_read = 0;

    // PROFILE.DAT: without it the macro files cannot be named
    char names[CAPE_SLOT_MACROS_MAX][5];
    uint32_t sizes[CAPE_SLOT_MACROS_MAX];
    unsigned nmacros = 0;
    int st = get(ctx, CAPE_FILE_DAT, buf, cap, &len);
    if(st < 0)
        return st;
    if(st > 0)
        file_why(why, sizeof(why), "PROFILE.DAT", st);
    else {
        const int n = cape_dat_parse(buf, len, NULL, 0);
        if(n < 0 || cape_dat_winlock(buf, len, &img->winlock) != CAPE_OK)
            put_reason(why, sizeof(why), "PROFILE.DAT is not understood (%s): the macro files it names cannot be kept",
                       cape_strerror(n < 0 ? n : CAPE_E_SIZE));
        else {
            dat_read = 1;
            cape_dat_entry* entries = n ? malloc((size_t)n * sizeof(*entries)) : NULL;
            if(n && !entries)
                return CAPE_E_CAP;
            if(n)
                cape_dat_parse(buf, len, entries, (size_t)n);   // the same bytes again: the same count
            for(int i = 0; i < n && !why[0]; i++){
                char name[5];
                cape_dat_name(&entries[i], name);
                if(cape_macro_filename_parse(name) < 0){   // M and three lower-case hex digits (iCUE: M009, M00a...)
                    put_reason(why, sizeof(why), "PROFILE.DAT entry %d names a file that is not a macro file (M and three hex digits)", i);
                    break;
                }
                unsigned known = 0;
                for(unsigned m = 0; m < nmacros; m++)
                    known |= !strcmp(names[m], name);
                if(known)
                    continue;
                if(nmacros >= CAPE_SLOT_MACROS_MAX){
                    put_reason(why, sizeof(why), "PROFILE.DAT names more than %d macro files", CAPE_SLOT_MACROS_MAX);
                    break;
                }
                memcpy(names[nmacros], name, sizeof(name));
                sizes[nmacros++] = entries[i].size;
            }
            free(entries);
            if(cape_slot_files_add(&img->files, CAPE_FILE_DAT, buf, len) != CAPE_OK)
                return CAPE_E_CAP;
        }
    }

    files_known(get, ctx, nmacros);

    // PROFILE.MAP
    st = get(ctx, CAPE_FILE_MAP, buf, cap, &len);
    if(st < 0)
        return st;
    if(st > 0){
        if(!why[0])
            file_why(why, sizeof(why), "PROFILE.MAP", st);
    } else {
        const int rc = cape_map_parse(buf, len, NULL, 0);
        if(rc < 0){
            if(!why[0])
                put_reason(why, sizeof(why), "PROFILE.MAP is not valid (%s)", cape_strerror(rc));
        } else if(cape_slot_files_add(&img->files, CAPE_FILE_MAP, buf, len) != CAPE_OK)
            return CAPE_E_CAP;
    }

    // The macro files it names, in the order of the table
    for(unsigned m = 0; m < nmacros && !why[0]; m++){
        st = get(ctx, names[m], buf, cap, &len);
        if(st < 0)
            return st;
        if(st > 0){
            char what[32];
            snprintf(what, sizeof(what), "%.4s, which PROFILE.DAT names,", names[m]);   // (a name is four characters)
            file_why(why, sizeof(why), what, st);
            break;
        }
        if(len != sizes[m])
            img->warnings++;   // kept as it is (the bindings are RAW)
        if(cape_slot_files_add(&img->files, names[m], buf, len) != CAPE_OK)
            return CAPE_E_CAP;
    }
    if(why[0]){
        cape_binding_broken(&img->bind, why, dat_read, img->winlock);
        return CAPE_OK;
    }
    return cape_binding_import_files(&img->files, &img->bind);
}

// PROFILE.I of a slot in use (a slot without a valid one cannot be rewritten: refused), into buf (CAPE_MAX_FILE_SIZE bytes)
static int read_info(slot_source get, void* ctx, cape_slot_image* img, uint8_t* buf){
    size_t len = 0;
    int rc = CAPE_OK;
    // PROFILE.I: 268 bytes; a file much larger than that is not one
    int st = get(ctx, CAPE_FILE_INFO, buf, CAPE_SLOT_INFO_CAP, &len);
    if(st < 0)
        return st;
    if(st > 0)
        refuse(img, st == 1 ? "PROFILE.I does not exist" : "PROFILE.I is larger than expected");
    else if((rc = cape_info_parse(buf, len, &img->info)) != CAPE_OK)
        refuse(img, "PROFILE.I is not valid (%s)", cape_strerror(rc));
    else {
        img->info_ok = 1;
        if(memcmp(img->info.guid, img->id.guid, sizeof(img->info.guid)) != 0 || img->info.cookie != img->id.cookie)
            img->warnings++;   // the slot table wins, as in hwload
        if(cape_slot_files_add(&img->files, CAPE_FILE_INFO, buf, len) != CAPE_OK)
            return CAPE_E_CAP;
    }
    return CAPE_OK;
}

// Reads everything that can be read of a slot in use: PROFILE.I, the bindings (their state is in img->bind) and the lighting with all
// its files
static int read_slot(slot_source get, void* ctx, cape_slot_image* img){
    uint8_t* buf = malloc(CAPE_MAX_FILE_SIZE);
    if(!buf)
        return CAPE_E_CAP;
    int rc = read_info(get, ctx, img, buf);
    if(rc != CAPE_OK)
        goto done;
    rc = read_bindings(get, ctx, img, buf);
    if(rc != CAPE_OK)
        goto done;
    // What the lighting is now, to tell a slot that already looks as asked, what a write removes, and what a save that does not change
    // it writes back
    light_ctx lc = { get, ctx, &img->light_files };
    const unsigned tx0 = get == session_get ? ((session_ctx*)ctx)->s->tx_packets : 0;
    rc = cape_light_read_all(&img->light, light_get, &lc);
    if(get == session_get)
        img->light.packets = ((session_ctx*)ctx)->s->tx_packets - tx0;   // what the lighting alone took
    if(rc == CAPE_OK && img->info_ok){
        cape_light_indicators(&img->info, img->light.ind);
        img->light.ind_ok = 1;
    }
done:
    free(buf);
    return rc;
}

static void image_init(cape_slot_image* out, const cape_slotid* id){
    memset(out, 0, sizeof(*out));
    cape_light_slot_init(&out->light);
    out->whole = 1;
    if(id)
        out->id = *id;
}

int cape_slot_read(cape_session* s, unsigned slot, cape_slot_image* out){
    if(!out)
        return CAPE_E_ARG;
    image_init(out, NULL);
    if(!s)
        return CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return CAPE_E_RANGE;
    const unsigned tx0 = s->tx_packets;
    out->id = s->slots[slot];
    if(cape_slotid_is_empty(&out->id))
        return CAPE_OK;
    out->present = 1;
    session_ctx c = { s, slot };
    const int rc = read_slot(session_get, &c, out);
    out->packets = s->tx_packets - tx0;
    return rc;
}

int cape_slot_read_head(cape_session* s, unsigned slot, cape_slot_image* out){
    if(!out)
        return CAPE_E_ARG;
    image_init(out, NULL);
    if(!s)
        return CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return CAPE_E_RANGE;
    const unsigned tx0 = s->tx_packets;
    out->id = s->slots[slot];
    if(cape_slotid_is_empty(&out->id))
        return CAPE_OK;
    out->present = 1;
    out->whole = 0;
    uint8_t* buf = malloc(CAPE_MAX_FILE_SIZE);
    if(!buf)
        return CAPE_E_CAP;
    session_ctx c = { s, slot };
    const int rc = read_info(session_get, &c, out, buf);
    free(buf);
    if(rc == CAPE_OK && out->info_ok){
        cape_light_indicators(&out->info, out->light.ind);
        out->light.ind_ok = 1;
    }
    out->packets = s->tx_packets - tx0;
    return rc;
}

int cape_slot_image_from_files(const cape_slot_files* files, const cape_slotid* id, cape_slot_image* out){
    if(!out)
        return CAPE_E_ARG;
    image_init(out, id);
    if(!files || !id)
        return CAPE_E_ARG;
    if(cape_slotid_is_empty(id))
        return CAPE_OK;
    out->present = 1;
    return read_slot(memory_get, (void*)files, out);
}

// ---------------------------------------------------------------------------------------------------------
// The plan

static int is_high_surrogate(uint16_t u){
    return u >= 0xd800 && u <= 0xdbff;
}

// The first units of a name, as many as fit in `max` without splitting a surrogate pair
static size_t cut_units(const uint16_t* s, size_t n, size_t max){
    if(n <= max)
        return n;
    return is_high_surrogate(s[max - 1]) ? max - 1 : max;
}

// The name in the GUI is the name of the slot, or the first units of it: the daemon and the GUI hold fewer units than PROFILE.I
// does. hwload hands over 16 units (15 if the 16th is the first half of a surrogate pair), and a name that comes back from the
// GUI with the name command is cut again by u16enc() (profile.c), which keeps 15 units (14 if the 15th is the first half of a pair)
static int name_is_same(const uint16_t* old, size_t nold, const uint16_t* edit, size_t nedit){
    if(nedit == nold)
        return !memcmp(old, edit, nedit * sizeof(*edit));
    if(nedit >= nold || nedit > CAPE_SLOT_GUI_NAME_UNITS)
        return 0;
    if(memcmp(old, edit, nedit * sizeof(*edit)) != 0)
        return 0;
    return nedit == cut_units(old, nold, CAPE_SLOT_GUI_NAME_UNITS) || nedit == cut_units(old, nold, CAPE_SLOT_GUI_NAME_UNITS - 1);
}

// The picture is the one the slot's lighting already makes
static int same_picture(const cape_light_slot* L, const cape_slot_edit* e){
    if(L->state != CAPE_LIGHT_STATIC && L->state != CAPE_LIGHT_EMPTY)
        return 0;
    static const uint8_t black[3] = { 0, 0, 0 };
    for(int cell = 0; cell < CAPE_CELLS; cell++){
        const int led = cape_light_led(cell);
        uint8_t want[3] = { 0, 0, 0 };
        if(led >= 0 && (size_t)led < e->leds){
            want[0] = e->r[led];
            want[1] = e->g[led];
            want[2] = e->b[led];
        }
        if(memcmp(L->state == CAPE_LIGHT_STATIC ? L->image.rgb[cell] : black, want, 3) != 0)
            return 0;
    }
    return 1;
}

// The files in iCUE's order: the macro files (ascending), PROFILE.DAT, PROFILE.I, PROFILE.MAP, the lighting. The macro files are those of
// macros (the slot's as read, or the ones built from the bindings to write)
static int assemble(cape_slot_files* out, const cape_slot_files* macros, const uint8_t* dat, size_t dat_len, const uint8_t* info,
                    size_t info_len, const uint8_t* map, size_t map_len, const cape_light_file* light, size_t nlight){
    // The macro files, sorted by name (a selection sort: there are few)
    size_t order[CAPE_SLOT_FILES_MAX];
    size_t nm = 0;
    if(macros){
        for(size_t i = 0; i < macros->n; i++){
            if(cape_macro_filename_parse(macros->file[i].name) >= 0)
                order[nm++] = i;
        }
    }
    for(size_t i = 0; i < nm; i++){
        size_t lo = i;
        for(size_t j = i + 1; j < nm; j++){
            if(strcmp(macros->file[order[j]].name, macros->file[order[lo]].name) < 0)
                lo = j;
        }
        const size_t t = order[i];
        order[i] = order[lo];
        order[lo] = t;
    }
    int rc = CAPE_OK;
    for(size_t i = 0; i < nm && rc == CAPE_OK; i++)
        rc = cape_slot_files_add(out, macros->file[order[i]].name, macros->file[order[i]].data, macros->file[order[i]].len);
    if(rc == CAPE_OK)
        rc = cape_slot_files_add(out, CAPE_FILE_DAT, dat, dat_len);
    if(rc == CAPE_OK)
        rc = cape_slot_files_add(out, CAPE_FILE_INFO, info, info_len);
    if(rc == CAPE_OK)
        rc = cape_slot_files_add(out, CAPE_FILE_MAP, map, map_len);
    for(size_t i = 0; i < nlight && rc == CAPE_OK; i++)
        rc = cape_slot_files_add(out, light[i].name, light[i].data, light[i].len);
    return rc;
}

// The lighting files of a slot as they were read, in the order iCUE writes them: each layer's .d, then .k (static and custom layers),
// then .r (static layers), then the count. Returns how many, or -1 if one is missing
static int kept_lighting(const cape_slot_image* old, cape_light_file* out){
    size_t n = 0;
    for(unsigned l = 0; l < old->light.layers && l < CAPE_LAYERS_MAX; l++){
        const cape_layer_kind kind = old->light.kind[l];
        const char* exts = kind == CAPE_LAYER_STATIC ? "dkr" : kind == CAPE_LAYER_CUSTOM ? "dk" : kind == CAPE_LAYER_PREDEFINED ? "d" : NULL;
        if(!exts)
            return -1;
        for(const char* e = exts; *e; e++){
            char name[10];
            if(cape_light_filename(l, *e, name) != CAPE_OK)
                return -1;
            const cape_slot_file* f = cape_slot_files_find(&old->light_files, name);
            if(!f || f->len > sizeof(out[n].data) || n >= CAPE_LIGHT_FILES_MAX)
                return -1;
            snprintf(out[n].name, sizeof(out[n].name), "%s", name);
            memcpy(out[n].data, f->data, f->len);
            out[n++].len = f->len;
        }
    }
    const cape_slot_file* cnt = cape_slot_files_find(&old->light_files, CAPE_FILE_LAYERCOUNT);
    if(!cnt || cnt->len > sizeof(out[n].data) || n >= CAPE_LIGHT_FILES_MAX)
        return -1;
    snprintf(out[n].name, sizeof(out[n].name), "%s", CAPE_FILE_LAYERCOUNT);
    memcpy(out[n].data, cnt->data, cnt->len);
    out[n++].len = cnt->len;
    return (int)n;
}

static int same_id(const cape_slotid* a, const cape_slotid* b){
    return !memcmp(a->guid, b->guid, sizeof(a->guid)) && a->cookie == b->cookie && !memcmp(a->flags, b->flags, sizeof(a->flags));
}

int cape_slot_plan_build(const cape_slot_image* old, const cape_slot_edit* edit, cape_slot_plan* plan, cape_slot_files* out){
    if(!old || !edit || !plan || (!edit->light_keep && (!edit->r || !edit->g || !edit->b)))
        return CAPE_E_ARG;
    memset(plan, 0, sizeof(*plan));
    if(out)
        cape_slot_files_free(out);

    cape_light_pack_result pk;
    memset(&pk, 0, sizeof(pk));
    int prc = CAPE_OK;
    if(!edit->light_keep){
        prc = cape_light_pack_ordered(edit->r, edit->g, edit->b, edit->leds, edit->order, edit->norder, &pk);
        if(prc != CAPE_OK && prc != CAPE_LIGHT_E_COLOURS)
            return prc;
        plan->colours = pk.colours;
        plan->layers = pk.layers;
        plan->ignored = pk.ignored;
    }
    const size_t nedit = edit->name ? edit->name_units : 0;
    if(nedit > CAPE_INFO_NAME_MAX){
        put_reason(plan->reason, sizeof(plan->reason), "the name is longer than a profile name can be (%zu units, at most %d)", nedit,
                   CAPE_INFO_NAME_MAX);
        return CAPE_OK;
    }
    // The slot must be the one the edit was made on (after a save that failed: the one it was before, which the copy kept then is)
    if(edit->base && !same_id(&old->id, edit->base)){
        snprintf(plan->reason, sizeof(plan->reason), "the slot is not the one the edit was made on: it changed since it was read");
        return CAPE_OK;
    }
    // The performance settings to write: the edit's, else the slot's, else iCUE's defaults (a new profile, a PROFILE.DAT or
    // PROFILE.I that was not read)
    cape_info defaults;
    cape_info_init(&defaults);
    const uint8_t wl_old = old->bind.winlock_read ? old->winlock : CAPE_WINLOCK_DEFAULT;
    const uint8_t* ind_old = old->info_ok ? old->info.tail : defaults.tail;
    const uint8_t wl_new = edit->has_winlock ? edit->winlock : wl_old;
    const uint8_t* ind_new = edit->has_ind ? edit->ind : ind_old;
    plan->winlock_changed = edit->has_winlock && edit->winlock != wl_old;
    plan->indicators_changed = edit->has_ind && memcmp(edit->ind, ind_old, sizeof(edit->ind)) != 0;
    const int perf_changed = plan->winlock_changed || plan->indicators_changed;

    // The bindings to write, against what the slot's are
    const cape_binding_model* nb = edit->bindings;
    const int bind_model = old->present && old->bind.state == CAPE_BIND_OK;
    if(nb && nb->state != CAPE_BIND_OK){
        snprintf(plan->reason, sizeof(plan->reason), "the bindings to write are not a model");
        return CAPE_OK;
    }
    if(edit->recreate && !nb){
        snprintf(plan->reason, sizeof(plan->reason), "there are no bindings to rebuild the slot's from");
        return CAPE_OK;
    }
    if(nb && old->present && !old->refused){
        if(bind_model && edit->recreate){
            snprintf(plan->reason, sizeof(plan->reason), "the bindings of the slot are a model: they are edited, not rebuilt from scratch");
            return CAPE_OK;
        }
        if(!bind_model && !edit->recreate){
            put_reason(plan->reason, sizeof(plan->reason), "the bindings of the slot are not a model (%s): only a rebuild from scratch replaces them",
                       old->bind.reason);
            return CAPE_OK;
        }
    }

    cape_info info;
    const cape_slot_file *dat_file = NULL, *map_file = NULL;
    uint8_t dat_new[CAPE_DAT_HEADER], map_new[CAPE_MAP_HEADER];
    cape_light_file kept[CAPE_LIGHT_FILES_MAX];
    int nkept = 0;
    // A slot that cannot be written back as it is: refused (PROFILE.I), or bindings that cannot even be kept and are not rebuilt
    const int unusable = old->present && (old->refused || (old->bind.state == CAPE_BIND_BROKEN && !(nb && edit->recreate)));
    if(old->present){
        if(!unusable && (!old->info_ok || (!nb && (!cape_slot_files_find(&old->files, CAPE_FILE_DAT)
                                                   || !cape_slot_files_find(&old->files, CAPE_FILE_MAP))))){
            snprintf(plan->reason, sizeof(plan->reason), "the slot was not read");
            return CAPE_OK;
        }
        if(!unusable){
            info = old->info;
            uint16_t units[CAPE_INFO_NAME_UNITS + 1];
            const int nold = cape_info_get_name(&info, units, sizeof(units) / sizeof(units[0]));
            plan->name_changed = nedit > 0 && !name_is_same(units, nold < 0 ? 0 : (size_t)nold, edit->name, nedit);
            if(nb){
                char why[CAPE_BIND_WHY];
                if(cape_binding_check(nb, bind_model ? &old->bind : NULL, why, sizeof(why)) != CAPE_OK){
                    put_reason(plan->reason, sizeof(plan->reason), "the bindings cannot be written: %s", why);
                    return CAPE_OK;
                }
                plan->bindings_changed = !(bind_model && cape_binding_equal(&old->bind, nb));
            }
            if(edit->light_keep){
                // The lighting as it is, whatever it is, if all its files were read
                nkept = old->light.complete ? kept_lighting(old, kept) : -1;
                if(nkept <= 0){
                    put_reason(plan->reason, sizeof(plan->reason), "the lighting of the slot cannot be kept (%s)",
                               old->light.state == CAPE_LIGHT_UNKNOWN ? old->light.why
                               : old->light.missing[0] ? old->light.missing : "its files were not all read");
                    return CAPE_OK;
                }
                plan->keeps_lighting = 1;
                plan->layers = old->light.layers;
                if(!plan->name_changed && !plan->bindings_changed && !perf_changed && !edit->force){
                    plan->verdict = CAPE_SLOT_SAME;
                    return CAPE_OK;
                }
            } else {
                // The buttons against the colours they will have: the ones to write
                uint8_t ind[CAPE_IND_COUNT][3];
                memcpy(ind, ind_new, sizeof(ind));
                const uint8_t (*to_write)[3] = (const uint8_t (*)[3])ind;   // (ISO C before C23 does not convert it by itself)
                plan->buttons_changed = cape_light_buttons_changed(edit->r, edit->g, edit->b, edit->leds,
                                                                   old->light.ind_ok || edit->has_ind ? to_write : NULL);
                if(old->light.state == CAPE_LIGHT_EFFECTS){
                    plan->replaces |= CAPE_REPLACES_EFFECTS;
                    plan->effect_layers = old->light.n_predefined + old->light.n_custom;
                } else if(old->light.state != CAPE_LIGHT_STATIC && old->light.state != CAPE_LIGHT_EMPTY){
                    plan->replaces |= CAPE_REPLACES_UNKNOWN;
                }
                // The picture is the slot's own: nothing to write if the name and the bindings are too (unless the caller wants it written
                // again), else the lighting stays as it is
                if(same_picture(&old->light, edit)){
                    if(!plan->name_changed && !plan->bindings_changed && !perf_changed && !edit->force){
                        plan->verdict = CAPE_SLOT_SAME;
                        return CAPE_OK;
                    }
                    nkept = kept_lighting(old, kept);
                    plan->keeps_lighting = nkept > 0;
                    if(nkept > 0)
                        plan->layers = old->light.layers;
                }
            }
        }
    }
    if(prc == CAPE_LIGHT_E_COLOURS && !plan->keeps_lighting){
        put_reason(plan->reason, sizeof(plan->reason), "%u colours other than black: a profile holds at most %d", pk.colours,
                   CAPE_LAYERS_MAX);
        return CAPE_OK;
    }
    if(unusable){
        snprintf(plan->reason, sizeof(plan->reason), "%s", old->refused ? old->reason : old->bind.reason);
        return CAPE_OK;
    }

    if(old->present){
        dat_file = cape_slot_files_find(&old->files, CAPE_FILE_DAT);
        map_file = cape_slot_files_find(&old->files, CAPE_FILE_MAP);
        plan->id = old->id;
        plan->id.cookie = (uint16_t)(old->id.cookie + 1);
        plan->id.flags[0] = 0x00;
        plan->id.flags[1] = 0x01;
        plan->verdict = CAPE_SLOT_WRITE;
    } else {
        // An empty slot: a profile as iCUE makes a new one, DAT and MAP without entries and PROFILE.I with its defaults
        static const uint8_t zero_guid[16] = { 0 };
        if(!edit->guid || !memcmp(edit->guid, zero_guid, sizeof(zero_guid))){
            snprintf(plan->reason, sizeof(plan->reason), "a new profile needs a GUID");
            return CAPE_OK;
        }
        if(nedit == 0){
            snprintf(plan->reason, sizeof(plan->reason), "a new profile needs a name");
            return CAPE_OK;
        }
        if(nb){
            char why[CAPE_BIND_WHY];
            if(cape_binding_check(nb, NULL, why, sizeof(why)) != CAPE_OK){
                put_reason(plan->reason, sizeof(plan->reason), "the bindings cannot be written: %s", why);
                return CAPE_OK;
            }
            cape_binding_model none;
            cape_binding_init(&none);
            plan->bindings_changed = !cape_binding_equal(&none, nb);
        }
        cape_info_init(&info);
        plan->name_changed = 1;
        if(cape_dat_build_ex(CAPE_WINLOCK_DEFAULT, NULL, 0, dat_new, sizeof(dat_new)) != CAPE_DAT_HEADER
                || cape_map_build(NULL, 0, map_new, sizeof(map_new)) != CAPE_MAP_HEADER){
            snprintf(plan->reason, sizeof(plan->reason), "the files of a new profile cannot be built");
            return CAPE_OK;
        }
        memcpy(plan->id.guid, edit->guid, sizeof(plan->id.guid));
        plan->id.cookie = 0;
        plan->id.flags[0] = 0x00;
        plan->id.flags[1] = 0x01;
        plan->verdict = CAPE_SLOT_NEW;
    }
    if(plan->name_changed && cape_info_set_name(&info, edit->name, nedit) != CAPE_OK){
        plan->verdict = CAPE_SLOT_REFUSED;
        snprintf(plan->reason, sizeof(plan->reason), "the name cannot be written to PROFILE.I");
        return CAPE_OK;
    }
    memcpy(info.guid, plan->id.guid, sizeof(info.guid));
    info.cookie = plan->id.cookie;
    memcpy(info.tail, ind_new, CAPE_IND_COUNT * 3);   // the indicator colours; the last bytes of the tail stay

    uint8_t info_bytes[CAPE_INFO_SIZE];
    if(cape_info_build(&info, info_bytes, sizeof(info_bytes)) != CAPE_INFO_SIZE){
        plan->verdict = CAPE_SLOT_REFUSED;
        snprintf(plan->reason, sizeof(plan->reason), "PROFILE.I cannot be built");
        return CAPE_OK;
    }
    cape_light_file built[CAPE_LIGHT_FILES_MAX];
    const cape_light_file* light = kept;
    size_t nlight = nkept > 0 ? (size_t)nkept : 0;
    if(!plan->keeps_lighting){
        if(cape_light_files_build(&pk, built, CAPE_LIGHT_FILES_MAX, &nlight) != CAPE_OK){
            plan->verdict = CAPE_SLOT_REFUSED;
            snprintf(plan->reason, sizeof(plan->reason), "the lighting files cannot be built");
            return CAPE_OK;
        }
        light = built;
    }
    // The bindings: the slot's files when they do what the edit asks (also those of a slot our own tools wrote, in their order and with
    // their names), else built as iCUE builds them
    cape_slot_files bound;
    memset(&bound, 0, sizeof(bound));
    const cape_slot_files* macros = old->present ? &old->files : NULL;
    if(nb && (plan->bindings_changed || !old->present || edit->recreate)){
        const int brc = cape_binding_build(nb, &bound);
        if(brc != CAPE_OK){
            cape_slot_files_free(&bound);
            plan->verdict = CAPE_SLOT_REFUSED;
            put_reason(plan->reason, sizeof(plan->reason), "the bindings cannot be built (%s)", cape_strerror(brc));
            return CAPE_OK;
        }
        macros = &bound;
        dat_file = cape_slot_files_find(&bound, CAPE_FILE_DAT);
        map_file = cape_slot_files_find(&bound, CAPE_FILE_MAP);
    }
    if(out){
        // The one PROFILE.DAT written, whichever it is (the slot's, one built from the bindings, a new one), has the Win Lock options
        // to write in its byte 1: a DAT kept as it is changes in that byte only
        const uint8_t* dat = dat_file ? dat_file->data : dat_new;
        const size_t dat_len = dat_file ? dat_file->len : sizeof(dat_new);
        uint8_t* dat_out = malloc(dat_len ? dat_len : 1);
        if(!dat_out){
            cape_slot_files_free(&bound);
            plan->verdict = CAPE_SLOT_REFUSED;
            snprintf(plan->reason, sizeof(plan->reason), "the files of the slot do not fit together (%s)", cape_strerror(CAPE_E_CAP));
            return CAPE_OK;
        }
        memcpy(dat_out, dat, dat_len);
        if(dat_len >= 2)
            dat_out[1] = wl_new;
        const int rc = assemble(out, macros, dat_out, dat_len, info_bytes,
                                sizeof(info_bytes), map_file ? map_file->data : map_new, map_file ? map_file->len : sizeof(map_new), light, nlight);
        free(dat_out);
        if(rc != CAPE_OK){
            cape_slot_files_free(out);
            plan->verdict = CAPE_SLOT_REFUSED;
            snprintf(plan->reason, sizeof(plan->reason), "the files of the slot do not fit together (%s)", cape_strerror(rc));
        }
    }
    cape_slot_files_free(&bound);
    return CAPE_OK;
}
