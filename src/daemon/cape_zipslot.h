#ifndef CAPE_ZIPSLOT_H
#define CAPE_ZIPSLOT_H

/*
 * A slot of the Corsair K95 RGB Platinum (USB 1b1c:1b2d) through its PROFILE.ZIP, the way iCUE treats one: the ZIP holds the
 * model of the profile (cape_profile.h), and every other file of the slot is made of that model (cape_compile.h). iCUE trusts the
 * ZIP, never the other files, and makes all of them again at every save; the daemon reads and writes a slot the same way.
 *
 * What the ZIP leaves out: the name and the GUID of the profile (PROFILE.I and the slot table) and the bytes of PROFILE.I that
 * are not indicator colours. Every firmware file, including animated lighting, is compiled from the ZIP model.
 *
 * Pure code: nothing here talks to the device. Memory: a model owns its strings (cape_profile_free), a set of files its data
 * (cape_slot_files_free), a ZIP is malloc()ed (free).
 */

#include "cape_profile.h"
#include "cape_compile.h"
#include "cape_slotfiles.h"

#include <stddef.h>
#include <stdint.h>

// What a slot's PROFILE.ZIP is to the daemon
typedef enum {
    CAPE_ZIP_OK = 0,
    CAPE_ZIP_NONE,            // the slot has none
    CAPE_ZIP_UNKNOWN,         // it holds a type, a class version or a value the compatibility level does not have: an unsupported iCUE
    CAPE_ZIP_CORRUPT,         // it cannot be read: not qCompress, not a zlib stream, not the XML of the level, over a limit
    CAPE_ZIP_UNCOMPILABLE,    // it can be read, and holds what the files of the keyboard cannot (cape_compile.h)
    CAPE_ZIP_STALE            // it is good, and the files of the slot are not those it makes (their total size says so)
} cape_zip_state;

// "ok", "none", "unknown", "corrupt", "uncompilable", "stale"
const char* cape_zip_state_name(cape_zip_state st);

// The model of a PROFILE.ZIP of len bytes, or of an XML document of len bytes (the ZIP decompressed). On failure *p is left empty
// and why (CAPE_PROFILE_WHY bytes, may be NULL) says what. Returns CAPE_OK, CAPE_PROFILE_E_UNKNOWN (an unsupported iCUE), CAPE_PROFILE_E_CORRUPT
// (anything else that is not a ZIP or a document of the level), CAPE_E_CAP (out of memory) or CAPE_E_ARG.
int cape_zipslot_model(const uint8_t* zip, size_t len, cape_profile_profile* p, char* why);
int cape_zipslot_model_xml(const char* xml, size_t len, cape_profile_profile* p, char* why);

// The PROFILE.ZIP of a model, as iCUE writes it (its XML, compressed at level 9), into *zip (malloc()ed, *len bytes). Returns
// CAPE_OK; CAPE_PROFILE_E_CORRUPT (a value the level cannot write), a CAPE_XML_E_* code or CAPE_E_RANGE (over a limit of the
// document or of the ZIP: a ZIP that the reader above would refuse is never made), CAPE_E_CAP (out of memory) or CAPE_E_ARG.
int cape_zipslot_zip(const cape_profile_profile* p, uint8_t** zip, size_t* len, char* why);

// The files a model makes, into out (initialised here; cape_slot_files_free it, also on failure): the macro files, PROFILE.DAT
// and PROFILE.MAP in the order of cape_binding_build(); PROFILE.I, a copy of info (info_len bytes, NULL: none) with the
// indicator colours of the model; then all lighting files of cape_compile_lighting_build() for the physical layout.
// lt describes the complete lighting set and is filled whatever the
// result (zeroed on failure). Returns CAPE_OK, CAPE_COMPILE_E_UNCOMPILABLE, CAPE_COMPILE_E_NOCELL (why says what), CAPE_E_CAP or
// CAPE_E_ARG.
int cape_zipslot_files(const cape_profile_profile* p, int layout, const uint8_t* info, size_t info_len, cape_slot_files* out,
                       cape_compile_lighting* lt, char* why);

// Whether a file is one of the lighting: "lghtcnt.cnt" or "lght_..."
int cape_zipslot_is_lighting(const char* name);

// The total size of the files of a slot whose files are those of made and lt (cape_zipslot_files) and whose PROFILE.ZIP has
// zip_len bytes: what "0e 17 03 00" says of it (the sum of the sizes of the files of the selected slot).
uint64_t cape_zipslot_bytes(const cape_slot_files* made, const cape_compile_lighting* lt, size_t zip_len);

#endif  // CAPE_ZIPSLOT_H
