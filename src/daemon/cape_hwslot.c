#include "cape_hwslot.h"

#include <stdio.h>
#include <string.h>

static int unreserved(uint8_t c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

int cape_hwslot_encode(const uint8_t* in, size_t len, char* out, size_t cap){
    static const char hex[] = "0123456789ABCDEF";
    if((!in && len) || !out || !cap)
        return CAPE_E_ARG;
    size_t n = 0;
    for(size_t i = 0; i < len; i++){
        const size_t need = unreserved(in[i]) ? 1 : 3;
        if(n + need >= cap)
            return CAPE_E_CAP;
        if(need == 1)
            out[n++] = (char)in[i];
        else {
            out[n++] = '%';
            out[n++] = hex[in[i] >> 4];
            out[n++] = hex[in[i] & 15];
        }
    }
    out[n] = '\0';
    return (int)n;
}

static int hexval(char c){
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int cape_hwslot_decode(const char* text, size_t len, uint8_t* out, size_t cap){
    if((!text && len) || (!out && cap))
        return CAPE_E_ARG;
    size_t n = 0;
    for(size_t i = 0; i < len; i++){
        uint8_t c = (uint8_t)text[i];
        if(c == '%'){
            if(i + 2 >= len)
                return CAPE_E_RANGE;
            const int hi = hexval(text[i + 1]), lo = hexval(text[i + 2]);
            if(hi < 0 || lo < 0)
                return CAPE_E_RANGE;
            c = (uint8_t)(hi << 4 | lo);
            if(unreserved(c))
                return CAPE_E_RANGE;   // one text, one encoding
            i += 2;
        } else if(!unreserved(c))
            return CAPE_E_RANGE;
        if(n >= cap)
            return CAPE_E_CAP;
        out[n++] = c;
    }
    return (int)n;
}

// As getid() and ":hwid" print it (notify.c, profile.c): the GUID as the host reads its first fields, and the next four bytes
void cape_hwslot_id(const uint8_t* entry, char out[CAPE_HWSLOT_ID_MAX]){
    if(!entry){
        snprintf(out, CAPE_HWSLOT_ID_MAX, "0");
        return;
    }
    unsigned int data1;
    unsigned short data2, data3, data4a;
    unsigned int rev;
    memcpy(&data1, entry, 4);
    memcpy(&data2, entry + 4, 2);
    memcpy(&data3, entry + 6, 2);
    memcpy(&data4a, entry + 8, 2);
    memcpy(&rev, entry + 16, 4);
    snprintf(out, CAPE_HWSLOT_ID_MAX, "{%08X-%04hX-%04hX-%04hX-%02X%02X%02X%02X%02X%02X}:%x", data1, data2, data3, data4a,
             entry[10], entry[11], entry[12], entry[13], entry[14], entry[15], rev);
}

static int hexdigits(const char* p, size_t n, uint32_t* v){
    *v = 0;
    for(size_t i = 0; i < n; i++){
        const char c = p[i];
        int d;
        if(c >= '0' && c <= '9')
            d = c - '0';
        else if(c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return 0;
        *v = *v << 4 | (uint32_t)d;
    }
    return 1;
}

int cape_hwslot_parse_id(const char* text, size_t len, uint8_t entry[CAPE_SLOTID_SIZE]){
    if(!text || !entry)
        return CAPE_E_ARG;
    if(len == 1 && text[0] == '0'){
        memset(entry, 0, CAPE_SLOTID_SIZE);
        return 0;
    }
    // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}:rev
    if(len < 40 || len > 47 || text[0] != '{' || text[9] != '-' || text[14] != '-' || text[19] != '-' || text[24] != '-'
            || text[37] != '}' || text[38] != ':')
        return CAPE_E_RANGE;
    uint32_t d1, d2, d3, d4, b, rev;
    if(!hexdigits(text + 1, 8, &d1) || !hexdigits(text + 10, 4, &d2) || !hexdigits(text + 15, 4, &d3) || !hexdigits(text + 20, 4, &d4)
            || !hexdigits(text + 39, len - 39, &rev))
        return CAPE_E_RANGE;
    const unsigned int u1 = d1, urev = rev;
    const unsigned short u2 = (unsigned short)d2, u3 = (unsigned short)d3, u4 = (unsigned short)d4;
    memcpy(entry, &u1, 4);
    memcpy(entry + 4, &u2, 2);
    memcpy(entry + 6, &u3, 2);
    memcpy(entry + 8, &u4, 2);
    for(int i = 0; i < 6; i++){
        if(!hexdigits(text + 25 + 2 * i, 2, &b))
            return CAPE_E_RANGE;
        entry[10 + i] = (uint8_t)b;
    }
    memcpy(entry + 16, &urev, 4);
    // Only the one way of writing it that cape_hwslot_id() has: the same entry must not have two ids
    char again[CAPE_HWSLOT_ID_MAX];
    cape_hwslot_id(entry, again);
    if(strlen(again) != len || memcmp(again, text, len) != 0)
        return CAPE_E_RANGE;
    return 1;
}

int cape_hwslot_utf8(const uint16_t* in, size_t units, uint8_t* out, size_t cap){
    if((!in && units) || (!out && cap))
        return CAPE_E_ARG;
    size_t n = 0;
    for(size_t i = 0; i < units; i++){
        uint32_t c = in[i];
        if(c == 0)
            return CAPE_E_RANGE;
        if(c >= 0xd800 && c < 0xdc00){
            if(i + 1 >= units || in[i + 1] < 0xdc00 || in[i + 1] >= 0xe000)
                return CAPE_E_RANGE;
            c = 0x10000 + ((c - 0xd800) << 10) + (uint32_t)(in[i + 1] - 0xdc00);
            i++;
        } else if(c >= 0xdc00 && c < 0xe000)
            return CAPE_E_RANGE;
        const size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if(n + need > cap)
            return CAPE_E_CAP;
        if(need == 1)
            out[n++] = (uint8_t)c;
        else if(need == 2){
            out[n++] = (uint8_t)(0xc0 | c >> 6);
            out[n++] = (uint8_t)(0x80 | (c & 0x3f));
        } else if(need == 3){
            out[n++] = (uint8_t)(0xe0 | c >> 12);
            out[n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
            out[n++] = (uint8_t)(0x80 | (c & 0x3f));
        } else {
            out[n++] = (uint8_t)(0xf0 | c >> 18);
            out[n++] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
            out[n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
            out[n++] = (uint8_t)(0x80 | (c & 0x3f));
        }
    }
    return (int)n;
}

int cape_hwslot_utf16(const uint8_t* in, size_t len, uint16_t* out, size_t cap){
    if((!in && len) || (!out && cap))
        return CAPE_E_ARG;
    size_t n = 0;
    for(size_t i = 0; i < len;){
        const uint8_t b = in[i];
        uint32_t c;
        size_t need;
        if(b < 0x80){
            c = b;
            need = 1;
        } else if(b >= 0xc2 && b < 0xe0){
            c = b & 0x1f;
            need = 2;
        } else if(b >= 0xe0 && b < 0xf0){
            c = b & 0x0f;
            need = 3;
        } else if(b >= 0xf0 && b < 0xf5){
            c = b & 0x07;
            need = 4;
        } else
            return CAPE_E_RANGE;
        if(i + need > len)
            return CAPE_E_RANGE;
        for(size_t j = 1; j < need; j++){
            if((in[i + j] & 0xc0) != 0x80)
                return CAPE_E_RANGE;
            c = c << 6 | (in[i + j] & 0x3f);
        }
        // overlong forms, surrogates, beyond Unicode, and the zero that would end a name
        if(c == 0 || (need == 3 && c < 0x800) || (need == 4 && (c < 0x10000 || c > 0x10ffff)) || (c >= 0xd800 && c < 0xe000))
            return CAPE_E_RANGE;
        const size_t units = c >= 0x10000 ? 2 : 1;
        if(n + units > cap)
            return CAPE_E_CAP;
        if(units == 1)
            out[n++] = (uint16_t)c;
        else {
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        }
        i += need;
    }
    return (int)n;
}
