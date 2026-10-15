#include "cape_hwsave.h"

#include <stdio.h>
#include <string.h>

void cape_hwsave_state_init(cape_hwsave_state* st){
    if(st)
        memset(st, 0, sizeof(*st));
}

void cape_hwsave_state_drop(cape_hwsave_state* st, unsigned slot){
    if(!st || slot >= CAPE_SLOTS || !st->has_snap[slot])
        return;
    cape_slot_image_free(&st->snap[slot]);
    memset(&st->snap[slot], 0, sizeof(st->snap[slot]));
    st->has_snap[slot] = 0;
}

void cape_hwsave_state_free(cape_hwsave_state* st){
    if(!st)
        return;
    for(unsigned s = 0; s < CAPE_SLOTS; s++)
        cape_hwsave_state_drop(st, s);
}

int cape_hwsave_check(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* out){
    return cape_hwsave_prepare(st, io, slot, edit, out, NULL);
}

int cape_hwsave_prepare(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit,
                       cape_hwsave_result* out, cape_hwsave_prepared* prepared){
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!st || !io || !edit)
        return out->err = CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return out->err = CAPE_E_RANGE;
    if(prepared && prepared->valid){
        out->plan = prepared->plan;
        out->from_snapshot = prepared->recovery;
        return CAPE_OK;
    }
    cape_session s;
    int rc = cape_session_open(&s, io);
    if(rc != CAPE_OK)
        return out->err = rc;
    cape_session_progress_begin_alone(&s, CAPE_PHASE_READ, CAPE_PROGRESS_READ_FILES);   // (reported only if io has a .progress)
    cape_slot_image local;
    const cape_slot_image* img = &st->snap[slot];
    int have_local = 0;
    if(st->has_snap[slot]){
        out->from_snapshot = 1;
    } else {
        rc = cape_slot_read_zip(&s, slot, &local);
        have_local = 1;
        img = &local;
    }
    if(prepared) cape_hwsave_prepared_free(prepared);
    if(rc == CAPE_OK)
        rc = cape_slot_plan_build(img, edit, &out->plan, prepared ? &prepared->files : NULL);
    if(rc == CAPE_OK && prepared && (out->plan.verdict == CAPE_SLOT_WRITE || out->plan.verdict == CAPE_SLOT_NEW)){
        if(have_local){
            prepared->base = local;
            have_local = 0;
        } else
            rc = cape_slot_image_from_zip_files(&img->files, &img->id, img->layout, &prepared->base);
        if(rc == CAPE_OK){
            prepared->plan = out->plan;
            prepared->valid = 1;
        } else cape_hwsave_prepared_free(prepared);
    }
    out->packets = s.tx_packets;
    out->err = rc;
    if(have_local)
        cape_slot_image_free(&local);
    cape_session_close(&s);
    return rc;
}

int cape_hwsave_run(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* out){
    return cape_hwsave_run_ex(st, io, slot, edit, out, NULL);
}

int cape_hwsave_run_ex(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slot_edit* edit,
                       cape_hwsave_result* out, cape_slot_image* image){
    if(image)
        memset(image, 0, sizeof(*image));
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!st || !io || !edit)
        return out->err = CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return out->err = CAPE_E_RANGE;
    cape_wsession w;
    int rc = cape_wsession_open(&w, io, (uint8_t)(1u << slot));
    if(rc != CAPE_OK)
        return out->err = rc;
    cape_session_phase(&w.s, CAPE_PHASE_READ);
    cape_session_progress_begin(&w.s, CAPE_PHASE_READ, CAPE_PROGRESS_READ_FILES);

    // The slot as it is, or as it was before a save of it went wrong
    cape_slot_image local;
    cape_slot_image* img = &st->snap[slot];
    int have_local = 0;
    if(st->has_snap[slot]){
        out->from_snapshot = 1;
    } else {
        rc = cape_slot_read_zip(&w.s, slot, &local);
        have_local = 1;
        img = &local;
    }
    cape_slot_files files;
    memset(&files, 0, sizeof(files));
    if(rc == CAPE_OK)
        rc = cape_slot_plan_build(img, edit, &out->plan, &files);
    if(rc != CAPE_OK || (out->plan.verdict != CAPE_SLOT_WRITE && out->plan.verdict != CAPE_SLOT_NEW)){
        // Nothing to write: a slot that is refused or already as asked (or an error of reading it)
        out->err = rc;
        goto done;
    }

    out->wrote = 1;
    cape_session_phase(&w.s, CAPE_PHASE_WRITE);
    cape_session_progress_begin(&w.s, CAPE_PHASE_WRITE, (unsigned)files.n);
    rc = cape_write_slot(&w, slot, &out->plan.id, &files, &out->write);
    if(rc == CAPE_OK){
        cape_session_phase(&w.s, CAPE_PHASE_VERIFY);
        cape_session_progress_begin(&w.s, CAPE_PHASE_VERIFY, (unsigned)files.n);
        rc = cape_write_verify(&w, slot, &out->plan.id, &files, &out->verify);
    }
    if(rc != CAPE_OK){
        out->err = rc;
        // The slot was cleared (or is not what was written): what it held is kept for the next save, and so is the fact that it held
        // nothing: a slot left half written has its identity but not its files, which a read refuses, and the next save has to see
        // the slot as it was and not as the failure left it
        if(have_local && out->write.slot != CAPE_WSLOT_UNCHANGED){
            st->snap[slot] = local;
            st->has_snap[slot] = 1;
            have_local = 0;
        }
        goto done;
    }
    // Written and read back: the daemon's copy of the slot from the files, and nothing left to keep
    (void)cape_hwload_slot_from_files(&files, &out->plan.id, &out->slot);
    out->slot_ok = 1;
    if(image && cape_slot_image_from_zip_files(&files, &out->plan.id, img->layout, image) != CAPE_OK){
        cape_slot_image_free(image);
        memset(image, 0, sizeof(*image));   // (out of memory: the daemon reads the slot again at the next load)
    }
    cape_hwsave_state_drop(st, slot);

done:
    out->packets = w.s.tx_packets;
    cape_slot_files_free(&files);
    if(have_local)
        cape_slot_image_free(&local);
    cape_wsession_close(&w);
    return out->err;
}

static int same_entry(const cape_slotid* a, const cape_slotid* b){
    return !memcmp(a->guid, b->guid, sizeof(a->guid)) && a->cookie == b->cookie && a->flag == b->flag;
}

int cape_hwsave_save_prepared(cape_hwsave_state* st, const cape_io* io, unsigned slot, cape_hwsave_prepared* prepared,
                             cape_hwsave_result* out, cape_slot_image* image){
    if(image) memset(image, 0, sizeof(*image));
    if(!out) return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!st || !io || !prepared || !prepared->valid || slot >= CAPE_SLOTS) return out->err = CAPE_E_ARG;
    out->plan = prepared->plan;
    cape_wsession w;
    int rc = cape_wsession_open(&w, io, (uint8_t)(1u << slot));
    if(rc != CAPE_OK) return out->err = rc;
    const cape_slotid* current = &w.s.slots[slot];
    const cape_slotid empty = {0};
    const int unchanged = same_entry(current, &prepared->base.id);
    const int recovering = prepared->recovery && (same_entry(current, &empty) || same_entry(current, &prepared->plan.id));
    if(!unchanged && !recovering){
        out->plan.verdict = CAPE_SLOT_REFUSED;
        snprintf(out->plan.reason, sizeof(out->plan.reason), "the slot changed after Check");
        goto done;
    }
    out->from_snapshot = prepared->recovery;
    out->wrote = 1;
    cape_session_phase(&w.s, CAPE_PHASE_WRITE);
    cape_session_progress_begin(&w.s, CAPE_PHASE_WRITE, (unsigned)prepared->files.n);
    rc = cape_write_slot(&w, slot, &prepared->plan.id, &prepared->files, &out->write);
    if(rc == CAPE_OK){
        cape_session_phase(&w.s, CAPE_PHASE_VERIFY);
        cape_session_progress_begin(&w.s, CAPE_PHASE_VERIFY, (unsigned)prepared->files.n);
        rc = cape_write_verify(&w, slot, &prepared->plan.id, &prepared->files, &out->verify);
    }
    if(rc != CAPE_OK){
        out->err = rc;
        if(out->write.slot != CAPE_WSLOT_UNCHANGED) prepared->recovery = 1;
        goto done;
    }
    (void)cape_hwload_slot_from_files(&prepared->files, &prepared->plan.id, &out->slot);
    out->slot_ok = 1;
    if(image && cape_slot_image_from_zip_files(&prepared->files, &prepared->plan.id, prepared->base.layout, image) != CAPE_OK){
        cape_slot_image_free(image);
        memset(image, 0, sizeof(*image));
    }
    cape_hwsave_state_drop(st, slot);
 done:
    out->packets = w.s.tx_packets;
    cape_wsession_close(&w);
    return out->err;
}

int cape_hwsave_erase(cape_hwsave_state* st, const cape_io* io, unsigned slot, const cape_slotid* base, cape_hwsave_result* out){
    if(!out)
        return CAPE_E_ARG;
    memset(out, 0, sizeof(*out));
    if(!st || !io || !base)
        return out->err = CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return out->err = CAPE_E_RANGE;
    cape_wsession w;
    int rc = cape_wsession_open(&w, io, (uint8_t)(1u << slot));
    if(rc != CAPE_OK){
        out->packets = w.s.tx_packets;
        return out->err = rc;
    }
    cape_session_progress_begin_alone(&w.s, CAPE_PHASE_WRITE, 1);
    if(!same_entry(&w.s.slots[slot], base)){
        // Erased as the user saw it, or not at all
        out->plan.verdict = CAPE_SLOT_REFUSED;
        snprintf(out->plan.reason, sizeof(out->plan.reason), "the slot is not the one it was read as: it changed since it was read");
    } else {
        // Also an empty slot: an erase that stopped between the clear and the marker leaves the table empty and the files of the slot
        // still there until the marker comes, and the sequence is the same every time (each save begins with it)
        out->plan.verdict = CAPE_SLOT_WRITE;
        out->wrote = 1;
        cape_session_phase(&w.s, CAPE_PHASE_WRITE);
        rc = cape_erase_slot(&w, slot, &out->write);
        out->err = rc;
        // Erased, or maybe: what a failed save kept of the slot is not what it is to be any more (the user asked for it empty)
        if(rc == CAPE_OK || out->write.slot != CAPE_WSLOT_UNCHANGED)
            cape_hwsave_state_drop(st, slot);
    }
    if(out->err == CAPE_OK && out->plan.verdict != CAPE_SLOT_REFUSED){
        cape_light_slot_init(&out->slot.light);   // an empty slot, as hwload reads one
        out->slot_ok = 1;
    }
    out->packets = w.s.tx_packets;
    cape_wsession_close(&w);
    return out->err;
}
