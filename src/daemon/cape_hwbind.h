#ifndef CAPE_HWBIND_H
#define CAPE_HWBIND_H

/*
 * The record of a slot of the K95 RGB Platinum for "get :hwbind" (hwslot1, section 1 of cape_hwslot.h): the words and their pages,
 * made once from what the daemon's cache holds for the slot and then served page by page. Pure code.
 */

#include "cape_hwslot.h"

typedef enum {
    CAPE_HWBIND_LIGHT_STATIC = 0,
    CAPE_HWBIND_LIGHT_EMPTY,
    CAPE_HWBIND_LIGHT_EFFECTS,
    CAPE_HWBIND_LIGHT_UNKNOWN
} cape_hwbind_light;

// What a record is made of
typedef struct {
    const uint8_t* entry;               // the slot table entry (CAPE_SLOTID_SIZE bytes); NULL for an empty slot
    const cape_binding_model* bind;     // the bindings (EMPTY for an empty slot)
    const uint16_t* name;               // the name (UTF-16); NULL if not known, and for an empty slot
    size_t name_units;
    const uint8_t* indicators;          // 12 bytes, the indicator colours of PROFILE.I; NULL if it was not read
    cape_hwbind_light light;
    unsigned layers;
    const char* readonly;               // why the slot cannot be rewritten at all (UTF-8), NULL if it can
} cape_hwbind_slot;

typedef struct {
    unsigned m, gen;
    char* words;          // the words, each followed by a NUL
    size_t* word;         // the offset of each word in words
    size_t nwords;
    size_t* first;        // the first word of each page, and nwords after the last page
    unsigned pages;
} cape_hwbind_record;

// Makes the record of slot m (1..3) at generation gen. Returns CAPE_OK, CAPE_E_ARG, CAPE_E_RANGE (a name that is not UTF-16, a
// model with an event it cannot say) or CAPE_E_CAP (out of memory). The record must be freed either way.
int cape_hwbind_make(const cape_hwbind_slot* s, unsigned m, unsigned gen, cape_hwbind_record* out);
void cape_hwbind_free(cape_hwbind_record* r);
// The line of page p (1..pages), newline included, NUL-terminated. Returns its length; CAPE_E_RANGE for a page that does not exist,
// CAPE_E_CAP if cap is too small (CAPE_HWSLOT_LINE_MAX + 1 is always enough).
int cape_hwbind_line(const cape_hwbind_record* r, unsigned p, char* out, size_t cap);
// "mode <m> hwbind <gen> error <why>\n". Returns its length or CAPE_E_CAP.
int cape_hwbind_error_line(unsigned m, unsigned gen, const char* why, char* out, size_t cap);

#endif
