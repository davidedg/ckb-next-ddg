#ifndef CAPE_SLOTFILES_H
#define CAPE_SLOTFILES_H

/*
 * A set of files of an on-board profile slot of the Corsair K95 RGB Platinum, in memory: what a slot was read as, or is to be written
 * as (cape_slot.h), and what its bindings are built into (cape_binding.h). The functions are in cape_slot.c.
 */

#include "cape_light.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_SLOT_NAME        12    // a file name and its terminator
// The most files of a slot this module handles: PROFILE.I, .DAT, .MAP, the macro files, and the lighting (CAPE_LIGHT_FILES_MAX)
// One macro file per key at most (152 keys, CAPE_BIND_K95_KEYS): a slot of iCUE with a macro on every key can be read and kept
#define CAPE_SLOT_MACROS_MAX  152
#define CAPE_SLOT_FILES_MAX   (3 + CAPE_SLOT_MACROS_MAX + CAPE_LIGHT_FILES_MAX)

typedef struct {
    char name[CAPE_SLOT_NAME];
    uint8_t* data;    // allocated here, freed by cape_slot_files_free()
    size_t len;
} cape_slot_file;

typedef struct {
    cape_slot_file file[CAPE_SLOT_FILES_MAX];
    size_t n;
} cape_slot_files;

void cape_slot_files_free(cape_slot_files* f);
// The file called name, or NULL.
const cape_slot_file* cape_slot_files_find(const cape_slot_files* f, const char* name);
// Adds a copy of a file. Returns CAPE_OK, CAPE_E_ARG, CAPE_E_CAP (no room for another file, or memory ran out) or CAPE_E_RANGE (a
// name that does not fit, or one that is there already).
int cape_slot_files_add(cape_slot_files* f, const char* name, const uint8_t* data, size_t len);

#endif  // CAPE_SLOTFILES_H
