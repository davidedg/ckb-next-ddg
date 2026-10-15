#ifndef CAPE_HWSAVE_H
#define CAPE_HWSAVE_H

/*
 * K95 RGB Platinum slot validation and writing, shared with offline simulated-device tests.
 * The staged GUI path calls cape_hwsave_prepare() at Check and retains the resulting base, plan and exact files.
 * A repeated Check returns that frozen result without USB. cape_hwsave_save_prepared() verifies the slot identity,
 * writes those files and verifies them byte for byte, retaining the prepared transaction after an error for explicit Retry.
 * Successful readback constructs the cache image from the verified files without reading them again.
 * Nothing here automatically repeats a write; the first failure ends the attempt and is reported.
 *
 * The standalone check/run helpers also retain the original slot image after a destructive failure. Their state owns
 * these snapshots until success, explicit drop or destruction. Staged transactions own their prepared files separately;
 * the caller releases them on Abort, owner close, detach, Erase or successful Save.
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

// The immutable result of Check, owned by the transaction until Abort or success.
typedef struct {
    int valid;
    int recovery;                    // our previous attempt may have cleared or partly written this slot
    cape_slot_image base;
    cape_slot_plan plan;
    cape_slot_files files;
} cape_hwsave_prepared;

static inline void cape_hwsave_prepared_free(cape_hwsave_prepared* p){
    if(!p) return;
    cape_slot_image_free(&p->base);
    cape_slot_files_free(&p->files);
    p->valid = p->recovery = 0;
}

// Free or zero-initialize prepared before the first call. Only WRITE/NEW produces a valid prepared plan.
int cape_hwsave_prepare(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit,
                       cape_hwsave_result* out, cape_hwsave_prepared* prepared);
int cape_hwsave_save_prepared(cape_hwsave_state* st, const cape_io* io, unsigned slot, cape_hwsave_prepared* prepared,
                             cape_hwsave_result* out, cape_slot_image* image);

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

// The erase of a slot (cape_erase_slot), in a write session for this slot only, if its entry of the slot table is still base (the
// slot as the user saw it): else plan.verdict is REFUSED (reason says why) and nothing is written; else WRITE, and wrote is 1, also
// for an empty slot (an erase that stopped between the clear and the marker leaves the table empty and the files there until the
// marker: the same sequence again ends it). Erased (or failed after the clear), the snapshot of the slot is dropped. After a success result->slot is an empty slot (slot_ok). io as for cape_hwsave_run(). Returns CAPE_OK (also for a
// refusal) or the error that stopped it (also in result->err; result->write says where).
int cape_hwsave_erase(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slotid* base, cape_hwsave_result* out);

#endif  // CAPE_HWSAVE_H
