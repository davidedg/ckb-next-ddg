#ifndef CAPE_PROGRESS_H
#define CAPE_PROGRESS_H

/*
 * How far along a save of the Corsair K95 RGB Platinum is, on a fixed scale of 0..CAPE_PROGRESS_TOTAL, so that the
 * daemon can print "hwsave progress <done> <total> <phase>" lines (profile_cape.c) without the GUI ever having to
 * know how a save of this keyboard works. Pure computation: no I/O, no knowledge of file names or
 * the keyboard, nothing but cape_phase (below) and how many files each phase is expected to touch.
 *
 * cape_phase is the three parts of a save (cape_hwsave_run, cape_hwsave.h): reading the slot as it is (or taking
 * the Retry snapshot), the write itself, and reading it back to verify. A session that never writes is READ throughout
 * and never reports a change of phase. hwload at attach reports no progress (profile_cape.c gives it no cape_io.progress);
 * the read of one slot on demand and the check of a save through hwslot do (hwslot1: "hwread progress"
 * and "hwsavecheck progress", the second only as a heartbeat for the GUI's guard), over the whole scale: see
 * cape_progress_begin_alone().
 * It lives here, not in cape.h, because the progress bands below are defined in terms of it; cape.h includes this
 * header and uses cape_phase for cape_io.phase and cape_io.progress.
 *
 * The scale is divided into three fixed bands, one per phase, widths chosen from a save measured on firmware 3.29
 * with the daemon's pauses (0.796 s reading, 7.541 s writing, 0.726 s reading back; within the write,
 * the erase-and-set-identity preamble takes about 35% of the write's own share): READ 0..90, WRITE 90..920 (of
 * which the first 290 are the preamble, the rest shared out over the files), VERIFY 920..1000. These are a guess
 * about THIS save, not a promise: a phase that turns out to have more
 * files than it was told only stops climbing at its own band's ceiling instead of running into the next one, and
 * a read whose real file count is never known up front is not what this measures either -- CAPE_PROGRESS_READ_FILES,
 * a stand-in for "a typical slot", is what the read before a write is told to expect.
 *
 * Two calls only. cape_progress_begin() is called once per phase, at the same three transitions as
 * cape_session_phase() (cape_hwsave_run: read after the session opens, write before cape_write_slot, verify before
 * cape_write_verify), with how many files that phase expects to touch. cape_progress_step() is called once per
 * file done: by cape_read_file() (cape.c), for every file it finishes reading, which covers both the read before a
 * write and the verify after one; and, once, by cape_write_slot() (cape_write.c), after the erase-and-identity
 * preamble and again after each file it writes. cape_progress_value() never goes down, however these are called
 * and in whatever order: a caller bug (a phase entered twice, or out of order) can only ever fail to advance the
 * bar, not move it backward.
 */

typedef enum { CAPE_PHASE_READ, CAPE_PHASE_WRITE, CAPE_PHASE_VERIFY } cape_phase;

#define CAPE_PROGRESS_TOTAL          1000u

#define CAPE_PROGRESS_READ_BASE         0u
#define CAPE_PROGRESS_READ_WIDTH       90u
#define CAPE_PROGRESS_WRITE_BASE       90u
#define CAPE_PROGRESS_WRITE_WIDTH     830u
#define CAPE_PROGRESS_VERIFY_BASE     920u
#define CAPE_PROGRESS_VERIFY_WIDTH     80u
// The part of the WRITE band spent before the first file (select, size and mode queries, clear, marker, set
// identity, select again): about a third of it, stepped once, by cape_write_slot(), before its file
// loop starts. The rest of the band is shared out over the files cape_progress_begin() was told to expect.
#define CAPE_PROGRESS_WRITE_PREP     290u

// The guess used for a read whose real file count is not known when it starts (a typical slot, five layers): see
// cape_progress_begin().
#define CAPE_PROGRESS_READ_FILES      19u

typedef struct {
    cape_phase phase;         // set by cape_progress_begin(); kept only so cape_progress_phase_name() has it
    unsigned base, width;     // the current phase's band: value is always base + something <= width
    unsigned prep;            // the part of width that is a single lump (WRITE only; 0 otherwise), stepped before any file
    unsigned expected;        // files expected in this phase, besides the lump; 0 is "one lump", never a division by zero
    unsigned steps;           // cape_progress_step() calls seen since the last cape_progress_begin()
    unsigned floor;           // the lowest cape_progress_value() may still return: never goes down
} cape_progress;

// Starts at 0, no phase entered yet: cape_progress_value() is 0 until the first cape_progress_begin().
void cape_progress_init(cape_progress* p);

// Enters phase, expecting "expected" files in it (cape_write_slot's preamble is not one of them: see
// CAPE_PROGRESS_WRITE_PREP above). Safe to call more than once: a phase re-entered restarts its own step count, but
// cape_progress_value() cannot fall below what was already reported.
void cape_progress_begin(cape_progress* p, cape_phase phase, unsigned expected);

// Enters phase as the only phase of a session that has no other (the read of one slot, a check): its band is the whole scale,
// 0..CAPE_PROGRESS_TOTAL, with "expected" files in it, and no lump.
void cape_progress_begin_alone(cape_progress* p, cape_phase phase, unsigned expected);

// One file done (or, right after entering WRITE and before its first file, the erase-and-identity preamble).
void cape_progress_step(cape_progress* p);
// A better count of the files of the current phase, learnt on the way (a read that has found out how many macro files the slot has):
// the steps already made stay, and the value never goes down.
void cape_progress_expect(cape_progress* p, unsigned expected);

// 0..CAPE_PROGRESS_TOTAL, monotonically non-decreasing across any sequence of the two calls above.
unsigned cape_progress_value(const cape_progress* p);

// "read", "write" or "verify": the word profile_cape.c prints after the two numbers.
const char* cape_progress_phase_name(cape_phase phase);

#endif  // CAPE_PROGRESS_H
