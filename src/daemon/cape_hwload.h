#ifndef CAPE_HWLOAD_H
#define CAPE_HWLOAD_H

/*
 * What the daemon's hardware profile load ("hwload") needs from the K95 RGB Platinum: for each of the three
 * slots, who it is (GUID and cookie, from the slot table), how it is called (the name in PROFILE.I) and what its
 * lighting shows (the layer files, see cape_light.h). The GUI shows the slots as the hardware modes M1, M2 and M3.
 *
 * This is a read-only user of the transport (cape.h): it reads PROFILE.I of each slot in use, and the rest of a slot
 * (bindings and lighting) when it is asked for, and sends nothing else. Like the modules below it, it knows nothing of the daemon's structures, so it can
 * be tested without a keyboard and without udev.
 *
 * The identifiers of the profile as a whole, and of a slot that holds no profile, do not exist on the keyboard:
 * iCUE keeps every slot as an independent profile. ckb-next models "a hardware profile with three modes", so it
 * needs a profile GUID, and something to call a slot that is empty. cape_hw_synth_id() makes them from the serial
 * number of the keyboard: stable, kept in RAM and never written to the keyboard.
 */

#include "cape.h"
#include "cape_light.h"
#include "cape_slot.h"

#include <stddef.h>
#include <stdint.h>

// Room for a mode name in the daemon's hwprofile (MD_NAME_LEN), in UTF-16 units. The names iCUE accepts are longer.
#define CAPE_HW_NAME_UNITS    16
// The name the daemon gives the hardware profile, which has none on the keyboard
#define CAPE_HW_PROFILE_NAME  "K95 Platinum"
// The "slot" argument of cape_hw_synth_id() that asks for the identity of the profile as a whole
#define CAPE_HW_PROFILE_ID    (-1)

typedef struct {
    int present;                          // the slot table says the slot holds a profile
    uint8_t id[CAPE_SLOTID_SIZE];         // its entry of the slot table as sent: GUID, cookie, "00 01" (zeros if empty)
    uint16_t name[CAPE_HW_NAME_UNITS];    // the first 16 UTF-16 units of the name in PROFILE.I, zero padded
    int name_ok;                          // PROFILE.I was read and accepted (name is empty if not)
    cape_light_slot light;                // the lighting files; the indicator colours are set if PROFILE.I was accepted
} cape_hwslot;

typedef struct {
    cape_hwslot slot[CAPE_SLOTS];
    unsigned warnings;                    // things that looked wrong but did not stop the load
    unsigned packets;                     // packets the session sent for this load
} cape_hwload;

// Reads what hwload needs from an open session (cape_session_open): the slots of whole_mask (bit i = slot index i) read whole
// (cape_slot_read: PROFILE.I, the bindings and the lighting with all its files), the others only their PROFILE.I
// (cape_slot_read_head: the identity, the name and the indicator colours; whole = 0 in the image, the lighting unknown), as iCUE reads
// them when it starts. The attach reads no slot whole (the bindings and the lighting of a slot are read when they are
// asked for, cape_hwload_read_one()). A slot whose GUID in the table is all zeros is empty: nothing is sent for it. A slot without a valid PROFILE.I keeps its identity and gets an empty name, with a warning, and the load goes on. The
// identity is always the one of the slot table; a GUID or cookie in PROFILE.I that differs is reported. The names are shortened to
// CAPE_HW_NAME_UNITS units without cutting a surrogate pair in two. Whatever the bindings and the lighting hold ends in their states
// and in lines of the log, and never stops the load.
// images, if not NULL, gets the three slots as read (an empty one has present = 0), which the caller frees with
// cape_hwload_images_free(); on failure they are freed here. Returns CAPE_OK, or the error that ended the session (then *out must not
// be used). log may be NULL.
int cape_hwload_read_all(cape_session* s, cape_hwload* out, cape_slot_image images[CAPE_SLOTS], unsigned whole_mask, cape_logfn log,
                         void* logctx);
// Every slot in use read whole, without the images
int cape_hwload_read(cape_session* s, cape_hwload* out, cape_logfn log, void* logctx);
// One slot, as cape_hwload_read_all() reads it: whole or its PROFILE.I only, into out->slot[slot] (the rest of *out is left as it
// is; its warnings are added to) and *img, which the caller frees with cape_slot_image_free() whatever is returned. Returns CAPE_OK,
// or the error that ended the session (then out->slot[slot] and *img are not to be used).
int cape_hwload_read_one(cape_session* s, unsigned slot, int whole, cape_hwload* out, cape_slot_image* img, cape_logfn log, void* logctx);
void cape_hwload_images_free(cape_slot_image images[CAPE_SLOTS]);

// The slot as cape_hwload_read() would give it, from the files of a slot and its entry of the table (a save that was written and
// read back: the daemon brings its copy up to date without reading again). present is 1, name and light are as hwload makes them
// (the name cut to CAPE_HW_NAME_UNITS, the lighting an image or the reason it is not one, the indicator colours of PROFILE.I), or,
// if PROFILE.I is not there or not valid, an empty name. Returns CAPE_OK, or CAPE_E_ARG.
int cape_hwload_slot_from_files(const cape_slot_files* files, const cape_slotid* id, cape_hwslot* out);

// A GUID and the "modified" bytes for something that has none on the keyboard, as the 20 bytes of a slot table
// entry. Stable across runs and releases (the GUI keeps them): a change here changes the hardware profile of
// everyone. which is CAPE_HW_PROFILE_ID (any negative value) for the profile as a whole or 0..2 for an empty
// slot. The GUID is made from the serial number and which; it cannot come out all zeros (that is an empty slot),
// and the code guards against it anyway. "modified" is, for the profile, a checksum of the three entries of the
// slot table, so that the GUI reads the profile again when one of the slots changes; for an empty slot it is
// zero. serial may be NULL, slots may be NULL for the identity of a slot.
void cape_hw_synth_id(const char* serial, int which, const cape_slotid slots[CAPE_SLOTS], uint8_t out[CAPE_SLOTID_SIZE]);

#endif  // CAPE_HWLOAD_H
