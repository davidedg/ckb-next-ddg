#include "cape_hwload.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static void say(cape_logfn log, void* ctx, cape_loglevel level, const char* fmt, ...){
    if(!log)
        return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log(ctx, level, msg);
}

// A name for the log: ASCII, anything else as '?'
static void ascii_name(const uint16_t* units, size_t n, char* out, size_t cap){
    size_t i = 0;
    for(; i < n && i + 1 < cap; i++)
        out[i] = units[i] >= 0x20 && units[i] < 0x7f ? (char)units[i] : '?';
    out[i] = '\0';
}

static int is_high_surrogate(uint16_t u){
    return u >= 0xd800 && u <= 0xdbff;
}

// The name and the indicator colours of PROFILE.I in the slot: the name cut to what the daemon holds, without splitting a surrogate
// pair. Returns the units kept; *shortened says the name was longer than the limit.
static size_t take_info(const cape_info* info, cape_hwslot* hs, int* shortened){
    uint16_t units[CAPE_INFO_NAME_UNITS + 1];
    int n = cape_info_get_name(info, units, sizeof(units) / sizeof(units[0]));
    size_t keep = n < 0 ? 0 : (size_t)n;
    *shortened = keep > CAPE_HW_NAME_UNITS;
    if(*shortened)
        keep = CAPE_HW_NAME_UNITS;
    // A high surrogate at the end has lost its other half (cut by the limit, or it never had one)
    if(keep && is_high_surrogate(units[keep - 1]))
        keep--;
    memset(hs->name, 0, sizeof(hs->name));
    memcpy(hs->name, units, keep * sizeof(units[0]));
    hs->name_ok = 1;
    cape_light_indicators(info, hs->light.ind);
    hs->light.ind_ok = 1;
    return keep;
}

// What the lighting of a slot amounts to, in the log. Anything that is not an image of static layers is a line in the log, not an
// error: the load goes on and the slot has no colours to show.
static void log_light(const cape_light_slot* L, unsigned slot, cape_hwload* out, cape_logfn log, void* logctx){
    switch(L->state){
    case CAPE_LIGHT_STATIC:
        say(log, logctx, CAPE_LOG_INFO, "slot %u: lighting: %u static layer%s, %u cells covered", slot + 1, L->layers, L->layers > 1 ? "s" : "",
            cape_light_covered(&L->image));
        break;
    case CAPE_LIGHT_EMPTY:
        say(log, logctx, CAPE_LOG_INFO, "slot %u: lighting: no layers", slot + 1);
        break;
    case CAPE_LIGHT_EFFECTS:
        if(L->n_predefined)
            say(log, logctx, CAPE_LOG_INFO, "slot %u: lighting: predefined effect %u (speed %u, direction %u), not shown", slot + 1,
                (unsigned)L->predefined[0], (unsigned)L->predefined[1], (unsigned)L->predefined[3]);
        else
            say(log, logctx, CAPE_LOG_INFO, "slot %u: lighting: %s, not shown", slot + 1, L->why);
        if(!L->complete){
            out->warnings++;
            say(log, logctx, CAPE_LOG_WARN, "slot %u: lighting: %s is missing, the effects cannot be kept by a save", slot + 1, L->missing);
        }
        break;
    default:
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: lighting not understood, not shown: %s", slot + 1, L->why);
        break;
    }
    if(L->dup_cells){
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: lighting: %u cells listed twice in a layer, each counted once", slot + 1, L->dup_cells);
    }
    if(L->state == CAPE_LIGHT_STATIC && cape_light_unmapped(&L->image)){
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: lighting: %u cells listed that no key has, which cannot be shown", slot + 1, cape_light_unmapped(&L->image));
    }
}

static void log_bindings(const cape_binding_model* b, unsigned slot, cape_hwload* out, cape_logfn log, void* logctx){
    switch(b->state){
    case CAPE_BIND_OK:
        say(log, logctx, CAPE_LOG_INFO, "slot %u: bindings: %u remap%s, %u macro%s", slot + 1, b->remaps, b->remaps == 1 ? "" : "s", b->macros,
            b->macros == 1 ? "" : "s");
        break;
    case CAPE_BIND_RAW:
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: bindings kept as they are, not a model: %s", slot + 1, b->reason);
        break;
    default:
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: bindings that cannot be kept: %s", slot + 1, b->reason);
        break;
    }
}

// What hwload makes of a slot read: its identity, its name and indicator colours (from PROFILE.I; a slot without a valid one has no
// name, with a warning), its lighting if it was read whole, and the lines of the log
static void take_slot(const cape_slot_image* img, unsigned slot, cape_hwload* out, cape_hwslot* hs, cape_logfn log, void* logctx){
    hs->present = 1;
    hs->light = img->light;
    if(img->refused){
        out->warnings++;
        say(log, logctx, CAPE_LOG_WARN, "slot %u: %s, the slot has no name", slot + 1, img->reason);
    } else {
        if(memcmp(img->info.guid, img->id.guid, sizeof(img->info.guid)) != 0 || img->info.cookie != img->id.cookie){
            out->warnings++;
            say(log, logctx, CAPE_LOG_WARN, "slot %u: the GUID or the cookie of PROFILE.I differ from the slot table, the table wins", slot + 1);
        }
        int shortened = 0;
        const size_t keep = take_info(&img->info, hs, &shortened);
        char text[CAPE_HW_NAME_UNITS + 1];
        ascii_name(hs->name, keep, text, sizeof(text));
        say(log, logctx, CAPE_LOG_INFO, "slot %u: \"%s\"%s, cookie %u", slot + 1, text, shortened ? " (shortened)" : "", (unsigned)img->id.cookie);
    }
    if(!img->whole){
        say(log, logctx, CAPE_LOG_INFO, "slot %u: bindings and lighting not read (read when the slot is asked for)", slot + 1);
        return;
    }
    log_bindings(&img->bind, slot, out, log, logctx);
    log_light(&hs->light, slot, out, log, logctx);
}

void cape_hwload_images_free(cape_slot_image images[CAPE_SLOTS]){
    if(!images)
        return;
    for(unsigned i = 0; i < CAPE_SLOTS; i++){
        cape_slot_image_free(&images[i]);
        memset(&images[i], 0, sizeof(images[i]));
    }
}

int cape_hwload_read_one(cape_session* s, unsigned slot, int whole, cape_hwload* out, cape_slot_image* img, cape_logfn log, void* logctx){
    if(img)
        memset(img, 0, sizeof(*img));
    if(!s || !out || !img)
        return CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return CAPE_E_RANGE;
    if(s->state == CAPE_S_REFUSED)
        return CAPE_E_FW;
    if(s->state != CAPE_S_READY)
        return CAPE_E_BROKEN;
    cape_hwslot* hs = &out->slot[slot];
    memset(hs, 0, sizeof(*hs));
    const cape_slotid* id = &s->slots[slot];
    cape_slotid_build(id, hs->id, sizeof(hs->id));
    cape_light_slot_init(&hs->light);
    if(cape_slotid_is_empty(id)){
        say(log, logctx, CAPE_LOG_INFO, "slot %u: empty", slot + 1);
        return whole ? cape_slot_read(s, slot, img) : cape_slot_read_head(s, slot, img);   // (no packet: an empty, whole image)
    }
    const int rc = whole ? cape_slot_read(s, slot, img) : cape_slot_read_head(s, slot, img);
    if(rc != CAPE_OK){
        say(log, logctx, CAPE_LOG_ERR, "slot %u: reading failed: %s (status 0x%02x)", slot + 1, cape_errstr(rc), (unsigned)s->last_status);
        return rc;
    }
    take_slot(img, slot, out, hs, log, logctx);
    return CAPE_OK;
}

int cape_hwload_read_all(cape_session* s, cape_hwload* out, cape_slot_image images[CAPE_SLOTS], unsigned whole_mask, cape_logfn log,
                         void* logctx){
    if(images)
        memset(images, 0, CAPE_SLOTS * sizeof(images[0]));
    if(!s || !out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(s->state == CAPE_S_REFUSED)
        return CAPE_E_FW;
    if(s->state != CAPE_S_READY)
        return CAPE_E_BROKEN;

    const unsigned tx_before = s->tx_packets;
    unsigned in_use = 0, whole = 0;
    for(unsigned slot = 0; slot < CAPE_SLOTS; slot++){
        cape_slot_image img;
        const int w = (whole_mask >> slot) & 1;
        const int rc = cape_hwload_read_one(s, slot, w, out, &img, log, logctx);
        if(rc != CAPE_OK){
            cape_slot_image_free(&img);
            cape_hwload_images_free(images);
            return rc;
        }
        if(img.present){
            in_use++;
            whole += (unsigned)w;
        }
        if(images)
            images[slot] = img;   // (moved: the caller frees it)
        else
            cape_slot_image_free(&img);
    }
    out->packets = s->tx_packets - tx_before;
    if(whole == in_use)
        say(log, logctx, CAPE_LOG_INFO, "read %u slots in use, %u packets, %u warnings", in_use, out->packets, out->warnings);
    else
        say(log, logctx, CAPE_LOG_INFO, "read %u slots in use, %u of them whole, %u packets, %u warnings", in_use, whole, out->packets,
            out->warnings);
    return CAPE_OK;
}

int cape_hwload_read(cape_session* s, cape_hwload* out, cape_logfn log, void* logctx){
    return cape_hwload_read_all(s, out, NULL, (1u << CAPE_SLOTS) - 1, log, logctx);
}

// ---------------------------------------------------------------------------------------------------------
// Identities that do not exist on the keyboard. The hashes are FNV-1a; their constants and the layout of
// the bytes that are hashed are part of the identity: do not change them.

static uint64_t fnv64(uint64_t h, const void* data, size_t n){
    const uint8_t* p = data;
    for(size_t i = 0; i < n; i++){
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint32_t fnv32(const uint8_t* p, size_t n){
    uint32_t h = 0x811c9dc5u;
    for(size_t i = 0; i < n; i++){
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static void put64(uint8_t* p, uint64_t v){
    for(int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

void cape_hw_synth_id(const char* serial, int which, const cape_slotid slots[CAPE_SLOTS], uint8_t out[CAPE_SLOTID_SIZE]){
    memset(out, 0, CAPE_SLOTID_SIZE);
    // "ckb-next k95p", 0, which (0xff for the profile), 0, then the serial number without its terminator. Two
    // hashes with different starting values make the 16 bytes of the GUID.
    static const char prefix[] = "ckb-next k95p";
    const uint8_t sep = 0;
    const uint8_t w = which < 0 ? 0xff : (uint8_t)which;
    const char* ser = serial ? serial : "";
    uint64_t h[2] = { 0xcbf29ce484222325ULL, 0x84222325cbf29ce4ULL };
    for(int i = 0; i < 2; i++){
        h[i] = fnv64(h[i], prefix, sizeof(prefix) - 1);
        h[i] = fnv64(h[i], &sep, 1);
        h[i] = fnv64(h[i], &w, 1);
        h[i] = fnv64(h[i], &sep, 1);
        h[i] = fnv64(h[i], ser, strlen(ser));
        put64(out + 8 * i, h[i]);
    }
    // Never all zeros: that is what an empty slot looks like
    int zero = 1;
    for(int i = 0; i < 16; i++)
        zero &= out[i] == 0;
    if(zero)
        out[0] = 1;

    if(which < 0 && slots){
        uint8_t table[CAPE_SLOTS * CAPE_SLOTID_SIZE];
        for(int i = 0; i < CAPE_SLOTS; i++)
            cape_slotid_build(&slots[i], table + i * CAPE_SLOTID_SIZE, CAPE_SLOTID_SIZE);
        uint32_t m = fnv32(table, sizeof(table));
        for(int i = 0; i < 4; i++)
            out[16 + i] = (uint8_t)(m >> (8 * i));
    }
}

// The files of a slot in memory, as the source of cape_light_read()
static int memory_file(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len){
    const cape_slot_file* f = cape_slot_files_find(ctx, name);
    if(!f || f->len > cap)
        return 1;
    memcpy(buf, f->data, f->len);
    *len = f->len;
    return 0;
}

int cape_hwload_slot_from_files(const cape_slot_files* files, const cape_slotid* id, cape_hwslot* out){
    if(!files || !id || !out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    cape_light_slot_init(&out->light);
    out->present = 1;
    cape_slotid_build(id, out->id, sizeof(out->id));
    const cape_slot_file* info = cape_slot_files_find(files, CAPE_FILE_INFO);
    cape_info parsed;
    if(info && cape_info_parse(info->data, info->len, &parsed) == CAPE_OK){
        int shortened = 0;
        (void)take_info(&parsed, out, &shortened);
    }
    // A source in memory has nothing to fail with: what the files do not say is a state of the lighting, as it is for hwload
    (void)cape_light_read(&out->light, memory_file, (void*)files);
    return CAPE_OK;
}
