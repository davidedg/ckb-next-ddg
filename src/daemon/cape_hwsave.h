#ifndef CAPE_HWSAVE_H
#define CAPE_HWSAVE_H

/*
 * hwsave of the Corsair K95 RGB Platinum (USB 1b1c:1b2d): the colours of a hardware mode of the GUI to a slot of the keyboard, and
 * the check that says what a save would do. The daemon's commands are thin wrappers around this, so that everything that matters
 * can be tested against the simulated keyboard: what is read, what is planned, what is sent, what is kept when a save goes wrong.
 *
 * A check reads the slot (or takes the snapshot below) and plans; it sends nothing that changes the keyboard, and it is for the
 * questions "is this slot empty, unchanged, refused, and what would a save remove?" that the GUI asks before it offers a save.
 *
 * A save is: the slot read whole, the plan (cape_slot.h), the write and the reading back (cape_write.h), all in one session, and afterwards the slot as the daemon keeps it
 * (cape_hwslot), worked out from the files that were written and read back, without another read. Nothing is retried here or
 * below: the first failure ends it and is reported.
 *
 * The snapshot. A save begins by emptying the slot: if it fails after that, what the slot held (the remaps, the macros, the
 * indicator colours) is no longer on the keyboard, and a save read again would find an empty slot and write a profile without
 * them. So the slot as it was read is kept, in memory, for the next save of that slot: whether the user asks for the same save
 * again (Retry) or changes the picture first, the files that are kept come from the snapshot. A slot that was empty is kept as
 * empty (a save that fails leaves it with its identity and without its files, which a read would refuse). It is dropped when a save of that
 * slot succeeds, when the daemon reads the keyboard again (cape_hwsave_state_drop) and when the daemon is done with the keyboard.
 * The user is told, by the caller, that the macros and remaps of a slot that failed can be lost if the daemon goes away first.
 */

#include "cape.h"
#include "cape_hwload.h"
#include "cape_slot.h"
#include "cape_write.h"

#include <stdint.h>

typedef struct {
    cape_slot_image snap[CAPE_SLOTS];   // the slot as it was read before a save that failed after the clear
    int has_snap[CAPE_SLOTS];
} cape_hwsave_state;

// A state with no snapshot. The memory a state holds is freed by cape_hwsave_state_free().
void cape_hwsave_state_init(cape_hwsave_state* st);
void cape_hwsave_state_free(cape_hwsave_state* st);
// Forgets the snapshot of one slot (a new hwload has read the keyboard as it is now, or the user gave the save up).
void cape_hwsave_state_drop(cape_hwsave_state* st, unsigned slot);

typedef struct {
    int from_snapshot;           // the slot was not read: the snapshot of a save that failed was used
    cape_slot_plan plan;         // what the save is (verdict REFUSED, SAME, WRITE, NEW), with the reason and the counts
    int wrote;                   // a save was attempted (verdict WRITE or NEW)
    cape_write_report write;     // how far the save got: stage, file, packets, status, and the state of the slot
    cape_write_report verify;    // and the reading back
    int err;                     // CAPE_OK, or what stopped it: a failure of the session, of the save (see write.stage),
                                 // CAPE_E_PROTO for a slot that reads back different
    cape_hwslot slot;            // after a save that was written and read back: the slot as hwload would read it now
    int slot_ok;                 // slot is valid
    unsigned packets;            // packets of the session in all
} cape_hwsave_result;

// The check of a slot: reads it (or takes the snapshot) and plans. io must be a way to the
// keyboard through which every packet is tried once and read commands may be repeated; the daemon binds it as it does for hwload.
// Returns CAPE_OK, also for a refusal (see the result), or the error that ended the session.
int cape_hwsave_check(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* out);

// The save of a slot. io must send every packet at most once (see cape_write.h: the daemon's usbsend() repeats a packet that
// times out, unboundedly, which a write must never do). Returns CAPE_OK if the slot was written and reads back as written,
// CAPE_OK too when the slot needed nothing (verdict SAME: wrote is 0) or was refused (verdict REFUSED, reason in the plan); else the
// error (also in result->err). The write session is opened for this slot only.
int cape_hwsave_run(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* out);
// The same, and after a save that was written and read back, *image (if not NULL) gets the slot as cape_slot_read() would read it now,
// made from the files that were written (cape_slot_image_from_files): the daemon's copy of the slot, bindings included. It is the
// caller's to free; it is left empty (present = 0) when the save did not end so.
int cape_hwsave_run_ex(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit,
                       cape_hwsave_result* out, cape_slot_image* image);

#endif  // CAPE_HWSAVE_H
