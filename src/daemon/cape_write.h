#ifndef CAPE_WRITE_H
#define CAPE_WRITE_H

/*
 * Writing a profile slot of the Corsair K95 RGB Platinum (USB 1b1c:1b2d): the save of iCUE (corsair-protocol devices/k95p.md#write-a-slot),
 * packet for packet, on top of the transport of cape.h.
 *
 * The sequence, which iCUE's saves follow and this code repeats (a status read after each step that has one):
 *
 *   07 17 0c SS                      select the slot
 *   0e 17 03 00, 0e 17 0b 00         the size of the slot, and the free bytes of the flash (bytes 4..7, u32 LE): the save stops here,
 *                                    before anything is written, if its files do not fit in the free sectors (CAPE_E_SPACE)
 *   07 15 SS 00 <zeros>              clear the slot's GUID       (the status can take about a second)
 *   07 17 0e                         transaction marker: the slot is now empty
 *   07 15 SS 00 <GUID> <cookie> 00 01   set the GUID
 *   07 17 0c SS
 *   for each file:  07 17 05 00 <name>, then bursts of up to five chunks 7f NN LL 00 <data>, each ended by 07 17 09, then 07 17 08
 *   07 17 0e                         end of the transaction
 *
 * What is different here from the read-only session, on purpose:
 *  - A session that can write is opened by cape_wsession_open(), which is the only thing that gives a session the packets above.
 *    A session from cape_session_open() (the one hwload and the dump use) has none of them on its list: it cannot send one.
 *  - Only the slots of the mask the session was opened with may be written (a save opens it for the one slot it writes): for any
 *    other slot cape_write_slot() answers CAPE_E_MASKED without sending a packet.
 *  - The write packets are on the list only while cape_write_slot() runs, only for the slot it was called for, and only in the
 *    shapes above (a lighting frame has the same 7f header as a chunk of a file, and nothing else could tell them apart).
 *  - Nothing is ever retried. The first thing that goes wrong (a packet that is not sent, a status that is not 0, a reply that is
 *    not the right one) closes the file that is open, once, ends the session and says where it stopped and in what state the slot
 *    is: not touched, or cleared and not whole. Whether to try again is for the user, who is asked; a loop over a flash chip is
 *    not something to leave to code, and a reset of the USB device in the middle of a save is worse.
 *
 * The timing between packets (11 ms, as iCUE never goes faster) is the io's business, as in the read-only code. Seen on
 * firmware 3.29, bootloader 3.03.
 */

#include "cape.h"
#include "cape_slot.h"

#include <stdint.h>

// The status of a write step is read once, as iCUE does, and again every CAPE_WRITE_STATUS_POLL_MS if it is not final: the
// erase of a slot takes about a second (a reply that comes late) and iCUE's own tool waited for status 0 for up to five seconds
#define CAPE_WRITE_STATUS_POLLS    50
#define CAPE_WRITE_STATUS_POLL_MS  100

typedef struct {
    cape_session s;    // the session, first: use &w->s for the reads (cape_read_file)
    uint8_t mask;      // the slots that may be written
    int target;        // the slot cape_write_slot() is writing, -1 the rest of the time: the write packets are refused then
} cape_wsession;

// Opens a session as cape_session_open() does (the same identification, the same refusal of a keyboard that is not a tested one,
// exactly four packets) and lets it write the slots of mask (bit i is slot index i). Returns what cape_session_open() returns.
int cape_wsession_open(cape_wsession* w, const cape_io* io, uint8_t mask);
void cape_wsession_close(cape_wsession* w);

// Where a save stopped
typedef enum {
    CAPE_WSTAGE_NONE = 0,
    CAPE_WSTAGE_PREPARE,     // the arguments, the mask, the state of the session: nothing was sent
    CAPE_WSTAGE_CLEAR,       // the selection, the size, "write mode", the clear of the GUID and the first marker
    CAPE_WSTAGE_IDENTITY,    // the GUID and cookie
    CAPE_WSTAGE_FILE,        // the files, one after the other: file says which
    CAPE_WSTAGE_MARKER,      // the marker that ends the transaction
    CAPE_WSTAGE_DONE,        // it finished
    CAPE_WSTAGE_VERIFY       // the reading back (cape_write_verify)
} cape_wstage;

// What is on the slot afterwards
typedef enum {
    CAPE_WSLOT_UNCHANGED = 0,   // no clear was sent: the slot is as it was
    CAPE_WSLOT_INCOMPLETE,      // the clear was (or may have been) sent and the save did not finish: the slot is not whole
    CAPE_WSLOT_WRITTEN          // the save finished
} cape_wslot_state;

typedef struct {
    cape_wstage stage;
    char file[CAPE_SLOT_NAME];   // the file being written or read back (stage FILE and VERIFY), else empty
    unsigned packets;            // packets the call sent
    int err;                     // CAPE_OK, or the CAPE_E_* code of what went wrong
    uint8_t status;              // the last status the keyboard gave
    cape_wslot_state slot;
    unsigned need_sectors, free_sectors;   // flash sectors of CAPE_SECTOR_SIZE the files take and the flash had free (0 before 0e 17 0b)
} cape_write_report;

// A file takes whole sectors of the flash, at least one (corsair-protocol formats/cape/README.md#space)
#define CAPE_SECTOR_SIZE 4096u
unsigned cape_file_sectors(size_t len);

const char* cape_wstage_name(cape_wstage stage);

// Writes slot `slot` as the files of `files` (in that order: the order iCUE writes them, which cape_slot_plan_build() gives) with
// the identity `id`. Before anything is sent it checks what it is given: the slot is in the mask (else CAPE_E_MASKED), the
// session is usable, id has a GUID and the "00 01" every entry of the table ends with, every name is valid and every file
// fits, and PROFILE.I is there with the GUID and cookie of id (CAPE_E_RANGE otherwise: a bug of the caller). Then the sequence
// above. The free space is checked the prudent way: the sectors of all the files must be free before the slot is cleared, the
// sectors its old files would free are not counted (iCUE's figure counts them as used: a slot that fits only with them is refused).
// rep (may be NULL) is filled whatever happens. Returns CAPE_OK, or the error that stopped it; after an error other than a refusal
// before the first packet the session is ended (also after CAPE_E_SPACE, which comes after the three packets that write nothing).
int cape_write_slot(cape_wsession* w, unsigned slot, const cape_slotid* id, const cape_slot_files* files, cape_write_report* rep);

// Reads the slot back and compares it with what was written: its entry of the slot table is id, and every file is byte for byte
// the one that was written. CAPE_OK, or CAPE_E_PROTO with rep->file saying which (or empty for the identity); a failure of the
// session is its own code. The session's own copy of the slot table is brought up to date.
int cape_write_verify(cape_wsession* w, unsigned slot, const cape_slotid* id, const cape_slot_files* files, cape_write_report* rep);

#endif  // CAPE_WRITE_H
