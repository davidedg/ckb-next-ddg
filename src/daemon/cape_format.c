#include "cape_format.h"

#include <limits.h>
#include <string.h>

static uint16_t rd16(const uint8_t* p){
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t* p, uint16_t v){
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

const char* cape_strerror(int err){
    switch(err){
    case CAPE_OK:
        return "ok";
    case CAPE_E_ARG:
        return "invalid argument";
    case CAPE_E_SIZE:
        return "length does not match the header";
    case CAPE_E_MAGIC:
        return "wrong container type or version";
    case CAPE_E_RANGE:
        return "value out of range";
    case CAPE_E_CAP:
        return "output buffer too small";
    default:
        return "unknown error";
    }
}

// PROFILE.MAP

int cape_map_parse(const uint8_t* buf, size_t len, cape_map_entry* out, size_t cap){
    if(!buf)
        return CAPE_E_ARG;
    if(len < CAPE_MAP_HEADER)
        return CAPE_E_SIZE;
    if(buf[0] != 0x41 || buf[1] != 0x00)
        return CAPE_E_MAGIC;
    size_t n = rd16(buf + 2);
    if(len != CAPE_MAP_HEADER + n * CAPE_MAP_ENTRY_SIZE)
        return CAPE_E_SIZE;
    if(!out)
        return (int)n;
    if(n > cap)
        return CAPE_E_CAP;
    for(size_t i = 0; i < n; i++){
        const uint8_t* p = buf + CAPE_MAP_HEADER + i * CAPE_MAP_ENTRY_SIZE;
        out[i].src = p[0];
        out[i].dst = p[1];
        out[i].flags = p[2];
    }
    return (int)n;
}

int cape_map_build(const cape_map_entry* in, size_t n, uint8_t* out, size_t cap){
    if(!out || (!in && n))
        return CAPE_E_ARG;
    if(n > 0xffff)
        return CAPE_E_RANGE;
    size_t need = CAPE_MAP_HEADER + n * CAPE_MAP_ENTRY_SIZE;
    if(need > cap)
        return CAPE_E_CAP;
    out[0] = 0x41;
    out[1] = 0x00;
    wr16(out + 2, (uint16_t)n);
    for(size_t i = 0; i < n; i++){
        uint8_t* p = out + CAPE_MAP_HEADER + i * CAPE_MAP_ENTRY_SIZE;
        p[0] = in[i].src;
        p[1] = in[i].dst;
        p[2] = in[i].flags;
    }
    return (int)need;
}

// PROFILE.DAT

// What a parse and a read of the Win Lock options have in common: the file is a DAT (byte 0) whose length is the
// one its entry count says. Byte 1, the Win Lock options, is not part of the check: any value goes.
static int dat_header(const uint8_t* buf, size_t len, size_t* n){
    if(!buf)
        return CAPE_E_ARG;
    if(len < CAPE_DAT_HEADER)
        return CAPE_E_SIZE;
    if(buf[0] != 0x50)
        return CAPE_E_MAGIC;
    *n = rd16(buf + 2);
    if(len != CAPE_DAT_HEADER + *n * CAPE_DAT_ENTRY_SIZE)
        return CAPE_E_SIZE;
    return CAPE_OK;
}

int cape_dat_winlock(const uint8_t* buf, size_t len, uint8_t* winlock){
    size_t n;
    if(!winlock)
        return CAPE_E_ARG;
    int rc = dat_header(buf, len, &n);
    if(rc != CAPE_OK)
        return rc;
    *winlock = buf[1];
    return CAPE_OK;
}

int cape_dat_parse(const uint8_t* buf, size_t len, cape_dat_entry* out, size_t cap){
    size_t n;
    int rc = dat_header(buf, len, &n);
    if(rc != CAPE_OK)
        return rc;
    if(!out)
        return (int)n;
    if(n > cap)
        return CAPE_E_CAP;
    for(size_t i = 0; i < n; i++){
        const uint8_t* p = buf + CAPE_DAT_HEADER + i * CAPE_DAT_ENTRY_SIZE;
        out[i].key = p[0];
        memcpy(out[i].name, p + 1, 4);
        out[i].size = (uint32_t)p[5] | ((uint32_t)p[6] << 8) | ((uint32_t)p[7] << 16);
        out[i].start = p[8];
        out[i].runtype = p[9];
        out[i].repeat = p[10];
        memcpy(out[i].reserved, p + 11, 5);
    }
    return (int)n;
}

int cape_dat_build(const cape_dat_entry* in, size_t n, uint8_t* out, size_t cap){
    return cape_dat_build_ex(CAPE_WINLOCK_DEFAULT, in, n, out, cap);
}

int cape_dat_build_ex(uint8_t winlock, const cape_dat_entry* in, size_t n, uint8_t* out, size_t cap){
    if(!out || (!in && n))
        return CAPE_E_ARG;
    if(n > 0xffff)
        return CAPE_E_RANGE;
    // Check everything before writing anything
    for(size_t i = 0; i < n; i++)
        if(in[i].size > CAPE_DAT_SIZE_MAX)
            return CAPE_E_RANGE;
    size_t need = CAPE_DAT_HEADER + n * CAPE_DAT_ENTRY_SIZE;
    if(need > cap)
        return CAPE_E_CAP;
    out[0] = 0x50;
    out[1] = winlock;
    wr16(out + 2, (uint16_t)n);
    for(size_t i = 0; i < n; i++){
        uint8_t* p = out + CAPE_DAT_HEADER + i * CAPE_DAT_ENTRY_SIZE;
        p[0] = in[i].key;
        memcpy(p + 1, in[i].name, 4);
        p[5] = (uint8_t)(in[i].size & 0xff);
        p[6] = (uint8_t)((in[i].size >> 8) & 0xff);
        p[7] = (uint8_t)((in[i].size >> 16) & 0xff);
        p[8] = in[i].start;
        p[9] = in[i].runtype;
        p[10] = in[i].repeat;
        memcpy(p + 11, in[i].reserved, 5);
    }
    return (int)need;
}

void cape_dat_name(const cape_dat_entry* entry, char out[5]){
    memcpy(out, entry->name, 4);
    out[4] = '\0';
}

// Macro files

int cape_macro_parse(const uint8_t* buf, size_t len, cape_macro_hdr* hdr, cape_event* out, size_t cap){
    if(!buf || !hdr)
        return CAPE_E_ARG;
    if(len < CAPE_MACRO_HEADER)
        return CAPE_E_SIZE;
    if(buf[0] != 'M')
        return CAPE_E_MAGIC;
    const uint32_t n = (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 8 | buf[4];
    // The events are 2 or 4 bytes each (cape_ev_size_of): walk them, they must end where the file ends
    size_t at = CAPE_MACRO_HEADER;
    for(uint32_t i = 0; i < n; i++){
        if(at >= len)
            return CAPE_E_SIZE;
        at += cape_ev_size_of(buf[at]);
    }
    if(at != len)   // also an event cut by the end of the file (at > len)
        return CAPE_E_SIZE;
    if(out && (size_t)n > cap)
        return CAPE_E_CAP;
    hdr->subtype = buf[1];
    memcpy(hdr->reserved, buf + 5, sizeof(hdr->reserved));
    if(out){
        at = CAPE_MACRO_HEADER;
        for(size_t i = 0; i < (size_t)n; i++){
            const unsigned size = cape_ev_size_of(buf[at]);
            cape_event e = { buf[at], buf[at + 1], size == 4 ? buf[at + 2] : 0, size == 4 ? buf[at + 3] : 0 };
            out[i] = e;
            at += size;
        }
    }
    return (int)n;
}

size_t cape_macro_bytes(const cape_event* ev, size_t n){
    size_t bytes = 0;
    for(size_t i = 0; ev && i < n; i++)
        bytes += cape_ev_size(ev[i]);
    return bytes;
}

int cape_macro_build(const cape_macro_hdr* hdr, const cape_event* ev, size_t n, uint8_t* out, size_t cap){
    if(!hdr || !out || (!ev && n))
        return CAPE_E_ARG;
    if(n > 0xffffff)
        return CAPE_E_RANGE;
    // The shortest the file can be first: n is checked against cap before any event is read
    if(CAPE_MACRO_HEADER + n * CAPE_EVENT_SIZE > cap)
        return CAPE_E_CAP;
    const size_t need = CAPE_MACRO_HEADER + cape_macro_bytes(ev, n);
    if(need > cap)
        return CAPE_E_CAP;
    out[0] = 'M';
    out[1] = hdr->subtype;
    out[2] = (uint8_t)(n >> 16);
    out[3] = (uint8_t)(n >> 8);
    out[4] = (uint8_t)n;
    memcpy(out + 5, hdr->reserved, sizeof(hdr->reserved));
    size_t at = CAPE_MACRO_HEADER;
    for(size_t i = 0; i < n; i++){
        const unsigned size = cape_ev_size(ev[i]);
        out[at] = ev[i].b0;
        out[at + 1] = ev[i].b1;
        if(size == 4){
            out[at + 2] = ev[i].b2;
            out[at + 3] = ev[i].b3;
        }
        at += size;
    }
    return (int)need;
}

int cape_macro_filename(unsigned idx, char out[5]){
    if(!out)
        return CAPE_E_ARG;
    if(idx > 0xfff)
        return CAPE_E_RANGE;
    static const char hex[] = "0123456789abcdef";
    out[0] = 'M';
    out[1] = hex[idx >> 8];
    out[2] = hex[(idx >> 4) & 0xf];
    out[3] = hex[idx & 0xf];
    out[4] = '\0';
    return CAPE_OK;
}

int cape_macro_filename_parse(const char* name){
    if(!name)
        return CAPE_E_ARG;
    if(name[0] != 'M')
        return CAPE_E_RANGE;
    int idx = 0;
    for(int i = 1; i < 4; i++){
        // a shorter name stops here at its terminator, so nothing past it is read
        char c = name[i];
        if(c >= '0' && c <= '9')
            idx = idx * 16 + (c - '0');
        else if(c >= 'a' && c <= 'f')
            idx = idx * 16 + (c - 'a' + 10);
        else
            return CAPE_E_RANGE;
    }
    if(name[4] != '\0')
        return CAPE_E_RANGE;
    return idx;
}

// PROFILE.I

int cape_info_parse(const uint8_t* buf, size_t len, cape_info* out){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len < 2)
        return CAPE_E_SIZE;
    if(buf[0] != 0x49 || buf[1] != 0x00)
        return CAPE_E_MAGIC;
    if(len != CAPE_INFO_SIZE)
        return CAPE_E_SIZE;
    memcpy(out->guid, buf + 2, 16);
    out->cookie = rd16(buf + 18);
    memcpy(out->pad, buf + 20, 2);
    memcpy(out->name_area, buf + 22, CAPE_INFO_NAME_AREA);
    memcpy(out->tail, buf + 22 + CAPE_INFO_NAME_AREA, CAPE_INFO_TAIL_SIZE);
    return CAPE_OK;
}

int cape_info_build(const cape_info* in, uint8_t* out, size_t cap){
    if(!in || !out)
        return CAPE_E_ARG;
    if(cap < CAPE_INFO_SIZE)
        return CAPE_E_CAP;
    out[0] = 0x49;
    out[1] = 0x00;
    memcpy(out + 2, in->guid, 16);
    wr16(out + 18, in->cookie);
    memcpy(out + 20, in->pad, 2);
    memcpy(out + 22, in->name_area, CAPE_INFO_NAME_AREA);
    memcpy(out + 22 + CAPE_INFO_NAME_AREA, in->tail, CAPE_INFO_TAIL_SIZE);
    return CAPE_INFO_SIZE;
}

void cape_info_init(cape_info* info){
    static const uint8_t tail[CAPE_INFO_TAIL_SIZE] = {
        0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    memset(info, 0, sizeof(*info));
    memcpy(info->tail, tail, sizeof(tail));
}

int cape_info_get_name(const cape_info* info, uint16_t* out, size_t cap_units){
    if(!info || !out)
        return CAPE_E_ARG;
    size_t n = 0;
    while(n < CAPE_INFO_NAME_UNITS && rd16(info->name_area + 2 * n) != 0)
        n++;
    if(cap_units < n + 1)
        return CAPE_E_CAP;
    for(size_t i = 0; i < n; i++)
        out[i] = rd16(info->name_area + 2 * i);
    out[n] = 0;
    return (int)n;
}

int cape_info_set_name(cape_info* info, const uint16_t* name, size_t units){
    if(!info || (!name && units))
        return CAPE_E_ARG;
    if(units > CAPE_INFO_NAME_MAX)
        return CAPE_E_RANGE;
    for(size_t i = 0; i < units; i++)
        if(name[i] == 0)
            return CAPE_E_RANGE;
    memset(info->name_area, 0, CAPE_INFO_NAME_AREA);
    for(size_t i = 0; i < units; i++)
        wr16(info->name_area + 2 * i, name[i]);
    return CAPE_OK;
}

// Slot identity

static void slotid_read(const uint8_t* p, cape_slotid* out){
    memcpy(out->guid, p, 16);
    out->cookie = rd16(p + 16);
    memcpy(out->flags, p + 18, 2);
}

int cape_slotid_parse(const uint8_t* buf, size_t len, cape_slotid* out){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_SLOTID_SIZE)
        return CAPE_E_SIZE;
    slotid_read(buf, out);
    return CAPE_OK;
}

int cape_slotid_build(const cape_slotid* in, uint8_t* out, size_t cap){
    if(!in || !out)
        return CAPE_E_ARG;
    if(cap < CAPE_SLOTID_SIZE)
        return CAPE_E_CAP;
    memcpy(out, in->guid, 16);
    wr16(out + 16, in->cookie);
    memcpy(out + 18, in->flags, 2);
    return CAPE_SLOTID_SIZE;
}

int cape_slotid_is_empty(const cape_slotid* id){
    if(!id)
        return 0;
    for(size_t i = 0; i < sizeof(id->guid); i++)
        if(id->guid[i])
            return 0;
    return 1;
}

int cape_slot_table_parse(const uint8_t* buf, size_t len, cape_slotid out[CAPE_SLOTS]){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_SLOTS * CAPE_SLOTID_SIZE)
        return CAPE_E_SIZE;
    for(size_t i = 0; i < CAPE_SLOTS; i++)
        slotid_read(buf + i * CAPE_SLOTID_SIZE, &out[i]);
    return CAPE_OK;
}

// Lighting

// The family that a descriptor of this size and type has, or CAPE_LDESC_UNKNOWN
static cape_ldesc_family family_of(size_t len, uint8_t type){
    if(len == CAPE_LIGHT_DESC_PREDEFINED
       && (type <= CAPE_FX_RAIN || type == CAPE_FX_TYPELIGHTINGGRADIENT || type == CAPE_FX_TYPELIGHTINGRIPPLE))
        return CAPE_LDESC_PREDEFINED;
    if(len == CAPE_LIGHT_DESC_STATIC && type == CAPE_FX_STATIC)
        return CAPE_LDESC_STATIC;
    if(len == CAPE_LIGHT_DESC_CUSTOM && type >= CAPE_FX_ADVANCEDSOLID && type <= CAPE_FX_ADVANCEDWAVE)
        return CAPE_LDESC_CUSTOM;
    return CAPE_LDESC_UNKNOWN;
}

cape_ldesc_family cape_light_desc_family(const uint8_t* buf, size_t len){
    if(!buf || len == 0)
        return CAPE_LDESC_UNKNOWN;
    return family_of(len, buf[0]);
}

const char* cape_fx_name(uint8_t effect){
    static const char* const names[] = {
        "ColorShift", "ColorPulse", "SpiralRainbow", "RainbowWave", "ColorWave", "Visor", "Rain", "Static",
        "TypeLightingGradient", "TypeLightingRipple", "AdvancedSolid", "AdvancedGradient", "AdvancedRipple", "AdvancedWave",
        "RecordedLighting"
    };
    return effect < sizeof(names) / sizeof(names[0]) ? names[effect] : "unknown";
}

int cape_light_desc_parse(const uint8_t* buf, size_t len, cape_light_desc* out){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_LIGHT_DESC_PREDEFINED && len != CAPE_LIGHT_DESC_STATIC)
        return CAPE_E_SIZE;
    if(family_of(len, buf[0]) == CAPE_LDESC_UNKNOWN)
        return CAPE_E_MAGIC;
    memset(out, 0, sizeof(*out));
    out->len = len;
    out->effect = buf[0];
    out->speed = buf[1];
    out->color_type = buf[2];
    out->direction = buf[3];
    memcpy(out->rest, buf + 4, len - 4);
    return CAPE_OK;
}

int cape_light_desc_build(const cape_light_desc* in, uint8_t* out, size_t cap){
    if(!in || !out)
        return CAPE_E_ARG;
    const cape_ldesc_family fam = family_of(in->len, in->effect);
    if(fam != CAPE_LDESC_PREDEFINED && fam != CAPE_LDESC_STATIC)
        return CAPE_E_RANGE;
    if(in->len > cap)
        return CAPE_E_CAP;
    out[0] = in->effect;
    out[1] = in->speed;
    out[2] = in->color_type;
    out[3] = in->direction;
    // (the family says 13 or 37 bytes, so this is at most the 33 of rest: said again where it is copied)
    if(in->len - 4 > sizeof(in->rest))
        return CAPE_E_RANGE;
    memcpy(out + 4, in->rest, in->len - 4);
    return (int)in->len;
}

void cape_light_desc_predefined(cape_light_desc* d, uint8_t effect, uint8_t speed, uint8_t direction){
    memset(d, 0, sizeof(*d));
    d->len = CAPE_LIGHT_DESC_PREDEFINED;
    d->effect = effect;
    d->speed = speed;
    d->color_type = CAPE_COLORTYPE_UNDEFINED;
    d->direction = direction;
}

void cape_light_desc_static(cape_light_desc* d){
    memset(d, 0, sizeof(*d));
    d->len = CAPE_LIGHT_DESC_STATIC;
    d->effect = CAPE_FX_STATIC;
    d->rest[18 - 4] = 1;
}

// The header of a custom effect, byte by byte (corsair-protocol formats/cape/custom-effects.md)
enum {
    CUS_TAIL = 1, CUS_VELOCITY = 2, CUS_TWO_SIDED = 3, CUS_UNKNOWN4 = 4, CUS_ANGLE = 5, CUS_ORIGIN_X = 9, CUS_ORIGIN_Y = 13,
    CUS_FLAGS = 17, CUS_DURATION = 21, CUS_STOP_AFTER = 23, CUS_RESERVED1 = 25, CUS_SAMPLE_COUNT = 37, CUS_RESERVED2 = 38
};

// An unsigned value read from the file as a signed one, without relying on how the compiler converts out of range
static int32_t as_i32(uint32_t v){
    return v > 0x7fffffffu ? -(int32_t)(0xffffffffu - v) - 1 : (int32_t)v;
}

int cape_light_custom_parse(const uint8_t* buf, size_t len, cape_light_custom* out){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_LIGHT_DESC_CUSTOM)
        return CAPE_E_SIZE;
    if(family_of(len, buf[0]) != CAPE_LDESC_CUSTOM)
        return CAPE_E_MAGIC;
    if(buf[CUS_SAMPLE_COUNT] != CAPE_LIGHT_SAMPLES)
        return CAPE_E_SIZE;
    memset(out, 0, sizeof(*out));
    out->effect = buf[0];
    out->tail = buf[CUS_TAIL];
    out->velocity = buf[CUS_VELOCITY];
    out->two_sided = buf[CUS_TWO_SIDED];
    out->unknown4 = buf[CUS_UNKNOWN4];
    out->angle = rd32(buf + CUS_ANGLE);
    out->origin_x = as_i32(rd32(buf + CUS_ORIGIN_X));
    out->origin_y = as_i32(rd32(buf + CUS_ORIGIN_Y));
    out->start_on_key_press = buf[CUS_FLAGS];
    out->start_with_profile = buf[CUS_FLAGS + 1];
    out->stop_on_key_press = buf[CUS_FLAGS + 2];
    out->stop_on_key_release = buf[CUS_FLAGS + 3];
    out->duration_100ms = rd16(buf + CUS_DURATION);
    out->stop_after_times = rd16(buf + CUS_STOP_AFTER);
    memcpy(out->reserved1, buf + CUS_RESERVED1, sizeof(out->reserved1));
    memcpy(out->reserved2, buf + CUS_RESERVED2, sizeof(out->reserved2));
    const uint8_t* p = buf + CAPE_LIGHT_CUSTOM_HEADER;
    for(size_t i = 0; i < CAPE_LIGHT_SAMPLES; i++, p += 4){
        out->samples[i].r = p[0];
        out->samples[i].g = p[1];
        out->samples[i].b = p[2];
        out->samples[i].a = p[3];
    }
    return CAPE_OK;
}

int cape_light_custom_build(const cape_light_custom* in, uint8_t* out, size_t cap){
    if(!in || !out)
        return CAPE_E_ARG;
    if(family_of(CAPE_LIGHT_DESC_CUSTOM, in->effect) != CAPE_LDESC_CUSTOM)
        return CAPE_E_RANGE;
    if(cap < CAPE_LIGHT_DESC_CUSTOM)
        return CAPE_E_CAP;
    out[0] = in->effect;
    out[CUS_TAIL] = in->tail;
    out[CUS_VELOCITY] = in->velocity;
    out[CUS_TWO_SIDED] = in->two_sided;
    out[CUS_UNKNOWN4] = in->unknown4;
    wr32(out + CUS_ANGLE, in->angle);
    wr32(out + CUS_ORIGIN_X, (uint32_t)in->origin_x);
    wr32(out + CUS_ORIGIN_Y, (uint32_t)in->origin_y);
    out[CUS_FLAGS] = in->start_on_key_press;
    out[CUS_FLAGS + 1] = in->start_with_profile;
    out[CUS_FLAGS + 2] = in->stop_on_key_press;
    out[CUS_FLAGS + 3] = in->stop_on_key_release;
    wr16(out + CUS_DURATION, in->duration_100ms);
    wr16(out + CUS_STOP_AFTER, in->stop_after_times);
    memcpy(out + CUS_RESERVED1, in->reserved1, sizeof(in->reserved1));
    out[CUS_SAMPLE_COUNT] = CAPE_LIGHT_SAMPLES;
    memcpy(out + CUS_RESERVED2, in->reserved2, sizeof(in->reserved2));
    uint8_t* p = out + CAPE_LIGHT_CUSTOM_HEADER;
    for(size_t i = 0; i < CAPE_LIGHT_SAMPLES; i++, p += 4){
        p[0] = in->samples[i].r;
        p[1] = in->samples[i].g;
        p[2] = in->samples[i].b;
        p[3] = in->samples[i].a;
    }
    return CAPE_LIGHT_DESC_CUSTOM;
}

int cape_keylist_parse(const uint8_t* buf, size_t len, uint8_t* out, size_t cap){
    if(!buf)
        return CAPE_E_ARG;
    if(len < 4)
        return CAPE_E_SIZE;
    uint32_t n = rd32(buf);
    if((size_t)n != len - 4)
        return CAPE_E_SIZE;
    if(n > (uint32_t)INT_MAX)
        return CAPE_E_RANGE;
    if(!out)
        return (int)n;
    if((size_t)n > cap)
        return CAPE_E_CAP;
    memcpy(out, buf + 4, n);
    return (int)n;
}

int cape_keylist_build(const uint8_t* keys, size_t n, uint8_t* out, size_t cap){
    if(!out || (!keys && n))
        return CAPE_E_ARG;
    if(n > (size_t)INT_MAX - 4)
        return CAPE_E_RANGE;
    if(n + 4 > cap)
        return CAPE_E_CAP;
    wr32(out, (uint32_t)n);
    if(n)
        memcpy(out + 4, keys, n);
    return (int)(n + 4);
}

int cape_colors_parse(const uint8_t* buf, size_t len, cape_rgba out[CAPE_COLORS_COUNT]){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_COLORS_SIZE)
        return CAPE_E_SIZE;
    for(size_t i = 0; i < CAPE_COLORS_COUNT; i++){
        out[i].r = buf[4 * i];
        out[i].g = buf[4 * i + 1];
        out[i].b = buf[4 * i + 2];
        out[i].a = buf[4 * i + 3];
    }
    return CAPE_OK;
}

int cape_colors_build(const cape_rgba in[CAPE_COLORS_COUNT], uint8_t* out, size_t cap){
    if(!in || !out)
        return CAPE_E_ARG;
    if(cap < CAPE_COLORS_SIZE)
        return CAPE_E_CAP;
    for(size_t i = 0; i < CAPE_COLORS_COUNT; i++){
        out[4 * i] = in[i].r;
        out[4 * i + 1] = in[i].g;
        out[4 * i + 2] = in[i].b;
        out[4 * i + 3] = in[i].a;
    }
    return CAPE_COLORS_SIZE;
}

int cape_layercount_parse(const uint8_t* buf, size_t len, uint32_t* out){
    if(!buf || !out)
        return CAPE_E_ARG;
    if(len != CAPE_LAYERCOUNT_SIZE)
        return CAPE_E_SIZE;
    *out = rd32(buf);
    return CAPE_OK;
}

int cape_layercount_build(uint32_t layers, uint8_t* out, size_t cap){
    if(!out)
        return CAPE_E_ARG;
    if(cap < CAPE_LAYERCOUNT_SIZE)
        return CAPE_E_CAP;
    wr32(out, layers);
    return CAPE_LAYERCOUNT_SIZE;
}

int cape_light_filename(unsigned layer, char ext, char out[10]){
    if(!out)
        return CAPE_E_ARG;
    if(layer > 9 || (ext != 'd' && ext != 'k' && ext != 'r'))
        return CAPE_E_RANGE;
    memcpy(out, "lght_0", 6);
    out[6] = (char)('0' + layer);
    out[7] = '.';
    out[8] = ext;
    out[9] = '\0';
    return CAPE_OK;
}
