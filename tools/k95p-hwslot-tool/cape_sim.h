#ifndef CAPE_SIM_H
#define CAPE_SIM_H

// A simulated K95 RGB Platinum behind cape_io, for the rehearsal of a write before it is made on the keyboard. It
// implements the commands of the read sequence the way the real keyboard answers them (the transcripts of real
// dumps), and it records everything it is sent. It also implements the sequence of a save (clear the slot's
// GUID, the transaction marker, set it, files in bursts of chunks, the marker), but only when it is told to
// (allow_write): otherwise a write command is a violation, which is how the read-only code proves that it sends
// none. Its model of the flash is what iCUE's saves show and nothing
// more: an erase at the first marker, a file made by opening it and committing bursts, and the statuses seen
// (0), so anything a real save never does is counted as a write error and not guessed at.
//
// Two deliberate properties:
//  - Every reply starts with random bytes and only the documented bytes are then written over them, so code
//    that reads an undocumented byte of a reply gets noise (the real keyboard reuses one reply buffer and
//    leaves bytes of earlier replies in it).
//  - It decides on its own, with a table written independently of cape_pkt_allowed(), which packets are
//    commands of the read sequence. Anything else, or a command with junk in the padding, counts as a
//    violation, so a mistake in the list of allowed packets cannot vouch for itself.

#include "cape.h"

#include <stddef.h>
#include <stdint.h>

#define SIM_MAX_FILES 32
#define SIM_NAME_MAX  16

typedef struct {
    char name[SIM_NAME_MAX];
    uint8_t* data;
    size_t len;
} sim_file;

typedef struct {
    uint8_t id[CAPE_SLOTID_SIZE];  // the entry of the slot table: GUID, cookie, "00 01"; all zeros = empty slot
    sim_file files[SIM_MAX_FILES];
    int nfiles;
} sim_slot;

typedef struct {
    // What it is
    uint8_t ident[CAPE_PKT_SIZE];  // reply to "0e 01"
    unsigned bufsize;              // reported by "0e 17 01"
    sim_slot slot[CAPE_SLOTS];
    int table_override;            // if set, the 60 bytes below are the slot table instead of the slot ids
    uint8_t table[CAPE_SLOTS * CAPE_SLOTID_SIZE];
    // The free space "0e 17 0b" answers (bytes 4..7, u32 LE, in bytes): the sectors of the flash less those the files of the three
    // slots take, a file ceil(len / 4096) sectors and at least one (4096-byte sectors); or free_sectors if free_forced
    unsigned flash_sectors;        // 2038, as ours (sim_init)
    int free_forced;
    unsigned free_sectors;

    // Misbehaviour. Packets are counted from 1, sends and transfers together, from the first packet of the sim.
    int allow_write;               // the write commands are accepted (else they are violations)
    unsigned busy_after_clear;     // after the clear of a GUID, this many status queries answer 02 (busy) before the real status
    unsigned corrupt_commit_at;    // the burst of this commit (counted from 1, over the life of the sim) is stored with its first byte flipped
    unsigned fail_at;              // this packet fails (returns -1) and has no effect
    unsigned corrupt_at;           // the reply to this packet has byte corrupt_byte flipped
    unsigned corrupt_byte;
    uint8_t force_status;          // status queries force_from .. force_from + force_count - 1 (counted from 1)
    unsigned force_from;           // answer force_status instead of the real status
    unsigned force_count;

    // State
    int selected;                  // slot selected for file operations, -1 = none
    int open_slot;
    int open_file;                 // index of the open file in its slot, -1 = none
    size_t pos;                    // bytes of the open file already moved to the read buffer
    uint8_t rbuf[CAPE_BURST_SIZE];
    size_t rbuf_len;
    int rbuf_fresh;                // the read buffer was filled and not all chunks were taken
    unsigned next_chunk;
    int erase_pending;             // a GUID was cleared and the marker that empties the slot has not come
    int erase_slot;
    int wopen;                     // index of the file open for writing in slot wslot, -1 = none
    int wslot;
    uint8_t wbuf[CAPE_BURST_SIZE]; // the chunks of the burst that is not committed yet
    size_t wbuf_len;
    unsigned wchunks;
    unsigned wnext;                // the number the next chunk must have
    unsigned markers;              // 07 17 0e since the clear of a GUID
    unsigned busy_left;
    uint8_t status;                // status of the last operation
    unsigned status_queries;
    unsigned sleeps;               // calls of sleep_ms
    unsigned sleep_ms_total;       // and the milliseconds they asked for
    uint64_t rng;

    // What it saw
    uint8_t (*tx)[CAPE_PKT_SIZE];
    unsigned ntx, tx_cap;
    unsigned violations;           // packets that are not commands of the read sequence, or have junk in the padding
    unsigned protocol_errors;      // read sequence used wrongly: chunk without a fresh buffer, chunk out of order...
    unsigned write_errors;         // save sequence used wrongly: a chunk with no file open, out of order, a file left open at the marker...
    unsigned clears, sets, erases, commits;   // what the save did
    unsigned io_mismatch;          // a command that gets a reply sent without one, or the other way round
    // The packets of a burst that go without a pause (cape_io.send_cont). The simulator decides on its own which packets may: a
    // chunk after the first of its burst (7f NN, NN >= 2) and a commit (07 17 09) that follows a chunk of its burst
    unsigned cont_packets;         // packets that came by send_cont
    unsigned cont_errors;          // ...of which were none of those
    int expect_cont;               // the writer under test has a send_cont: a packet that may go without a pause and came by send is a miss
    unsigned cont_missed;          // (counted only if expect_cont)
} cape_sim;

// A keyboard as verified: 1b1c:1b2d, firmware 3.29, bootloader 3.03, buffer 300, three empty slots.
void sim_init(cape_sim* k, uint64_t seed);
void sim_free(cape_sim* k);
// The free sectors "0e 17 0b" would answer now
unsigned sim_free_sectors(const cape_sim* k);
void sim_set_ident(cape_sim* k, uint16_t vid, uint16_t pid, uint16_t app_bcd, uint16_t bld_bcd);
// Makes the slot non-empty: GUID, cookie and the "00 01" that ends every entry.
void sim_set_slot(cape_sim* k, int slot, const uint8_t guid[16], uint32_t cookie);
// Adds a file to a slot (the data is copied). Returns 0, or -1 if the slot is full.
int sim_add_file(cape_sim* k, int slot, const char* name, const uint8_t* data, size_t len);
// A file that a previous session left open in that slot (it must exist).
void sim_leave_open(cape_sim* k, int slot, const char* name);
cape_io sim_io(cape_sim* k);

// Packets sent so far whose first byte is b0 (and, if b1 >= 0, whose second byte is b1 and third b2)
unsigned sim_count(const cape_sim* k, int b0, int b1, int b2);

#endif  // CAPE_SIM_H
