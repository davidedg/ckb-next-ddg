#ifndef CAPE_H
#define CAPE_H

/*
 * Read-only access to the on-board profile file system ("CAPE", protocol field 0x17) of the Corsair K95 RGB
 * Platinum (USB 1b1c:1b2d).
 *
 * This module speaks the protocol but does not know how to reach the keyboard: the caller supplies a
 * cape_io, two functions that send a 64-byte packet and, for commands that get an answer, read the 64-byte
 * reply. The daemon binds them to usbsend()/usbrecv(); a test can bind them to a simulated keyboard. It has
 * no dependencies besides the C library and two modules of pure computation (cape_format.h, cape_fw.h), allocates
 * nothing and logs nothing.
 *
 * Safety rules, enforced here and not left to the callers:
 *  - Nothing is sent before the keyboard has identified itself as a tested one (cape_fw.h). The first
 *    packet of a session is the "0e 01" identification request, which the daemon sends at every attach
 *    anyway. If the reply is not a tested device the session is refused and no other packet is ever sent.
 *  - Every packet leaves through cape_tx()/cape_txrx(), which refuse anything that is not on the list of
 *    read-only commands in cape_pkt_allowed(). In particular no file can be opened for writing, no data
 *    chunk can be sent and the slot GUIDs cannot be changed: those commands are simply not on the list.
 *  - The first failure of any kind (transport, unexpected reply, bad status) ends the session: a file that
 *    is open gets one "close" command and every later call returns CAPE_E_BROKEN without sending anything.
 *    There are no retries at this level. A missing file (status 4) is not a failure.
 *  - Only the documented bytes of a reply are ever interpreted: the firmware reuses one reply buffer, so
 *    the rest of a reply contains bytes of earlier replies.
 *
 * The sequences are the ones iCUE uses to read a slot (corsair-protocol devices/k95p.md#sequences). Seen on firmware 3.29,
 * bootloader 3.03 only.
 */

#include "cape_format.h"
#include "cape_fw.h"
#include "cape_progress.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_PKT_SIZE       64
#define CAPE_BURST_SIZE     300   // bytes the keyboard hands out per "07 17 0a", as reported by "0e 17 01"
#define CAPE_CHUNK_SIZE     60    // bytes per "ff NN LL 00" chunk, five chunks per burst
#define CAPE_MAX_FILE_SIZE  (256 * 1024)
#define CAPE_NAME_MAX       11    // longest file name (PROFILE.MAP, lghtcnt.cnt); 12 bytes with the terminator
#define CAPE_STATUS_POLLS   50    // how many times a status that is not final is read again
#define CAPE_STATUS_POLL_MS 20

// Errors, in addition to the CAPE_E_* codes of cape_format.h (-1 to -5)
#define CAPE_E_IO         (-10)  // the packet could not be sent or no reply came
#define CAPE_E_FW         (-11)  // not a tested device or firmware level: nothing was or will be sent
#define CAPE_E_PROTO      (-12)  // a reply is not what the protocol says (header, buffer size, slot table, size)
#define CAPE_E_STATUS     (-13)  // "0e 17 0d" reported an error, see last_status
#define CAPE_E_NOTFOUND   (-14)  // the file does not exist (status 4 on open); the session stays usable
#define CAPE_E_TIMEOUT    (-15)  // the status stayed at an unknown non-zero value for CAPE_STATUS_POLLS reads
#define CAPE_E_FORBIDDEN  (-16)  // packet not on the read-only list: a bug of the caller
#define CAPE_E_BROKEN     (-17)  // the session is not open, was closed, or ended after a failure
#define CAPE_E_OUT        (-18)  // an output (a directory or a file) could not be written
#define CAPE_E_MASKED     (-19)  // the slot is not in the mask of the write session (cape_write.h): nothing was sent
#define CAPE_E_SPACE      (-20)  // the files of a save do not fit in the free sectors of the flash (cape_write.h): nothing was written

// Short description of any CAPE_OK / CAPE_E_* code, for log messages.
const char* cape_errstr(int err);

// Where the modules above the transport (the dump, the hardware profile load) send their messages: one line of
// text, no newline. A NULL function means no messages.
typedef enum { CAPE_LOG_INFO, CAPE_LOG_WARN, CAPE_LOG_ERR } cape_loglevel;
typedef void (*cape_logfn)(void* ctx, cape_loglevel level, const char* msg);

// cape_phase (the three parts of a save) and the pure progress-band math that goes with it now live in
// cape_progress.h, included above: cape_io.phase and cape_io.progress below both use cape_phase to say where a
// save is.

// The two ways to reach the keyboard. Both return 0 on success and any other value on failure. pkt and reply
// are CAPE_PKT_SIZE bytes. sleep_ms may be NULL (no waiting between status polls).
//
// Build one with designated initializers (.ctx = ..., .send = ...): a field added at the end is then NULL and no site changes.
typedef struct {
    void* ctx;
    int (*send)(void* ctx, const uint8_t* pkt);                      // "07 ..." commands: no reply
    int (*xfer)(void* ctx, const uint8_t* pkt, uint8_t* reply);      // "0e ..." and "ff ..." commands: one reply
    void (*sleep_ms)(void* ctx, unsigned ms);
    // Optional. Like send, for a packet that continues a burst of a file write (a chunk after the first of its burst, and the commit
    // that follows a chunk): the sender does not wait before it. iCUE's packets of a burst are one USB transfer apart (~1 ms; a
    // transfer of ours lasts 0.4 to 1.3 ms), while the wait before any other packet is ~11 ms, and a chunk gets no reply that
    // there would be anything to wait for. NULL: send is used, with whatever it waits for.
    int (*send_cont)(void* ctx, const uint8_t* pkt);
    // Optional. Called by cape_hwsave_run() (cape_hwsave.h) when it moves from reading the slot to writing it, and again when it
    // moves from writing to reading it back, so the caller can pick a different pause for each (the write itself keeps iCUE's own
    // pace; the reads around it may use a shorter one, which worked on the keyboard). A session that never writes (hwload, a
    // check) never calls this: the pause set before it opened is the only signal it needs. NULL: no effect.
    void (*phase)(void* ctx, cape_phase p);
    // Optional. How far a save has gotten, on the scale of cape_progress.h: done, out of total (always CAPE_PROGRESS_TOTAL) and
    // the name of the current phase ("read", "write" or "verify"; cape_progress_phase_name()). Called by
    // cape_session_progress_begin() at the same three transitions as .phase above, by cape_session_progress_step() for every file
    // done, and by cape_session_progress_beat() to repeat the current line, unchanged, while a status poll is not resolving (so a
    // long, legitimate wait for the flash is not silence: cape_wait_status()). A save's own session has three phases;
    // hwload has no phases and is never given a .progress; the read of one slot and a check of hwslot are, with one phase over
    // the whole scale (cape_session_progress_begin_alone()). NULL: no effect.
    void (*progress)(void* ctx, unsigned done, unsigned total, const char* phase);
} cape_io;

typedef enum {
    CAPE_S_CLOSED,   // not opened, or closed
    CAPE_S_OPENING,
    CAPE_S_READY,
    CAPE_S_REFUSED,  // the keyboard is not a tested one
    CAPE_S_BROKEN    // ended after a failure
} cape_state;

typedef struct cape_session {
    cape_io io;
    cape_state state;
    int slot;                        // slot selected for file operations, -1 until the first selection
    int file_open;                   // a file is (or may be) open on the keyboard
    cape_fwinfo fw;                  // what the keyboard said about itself
    cape_slotid slots[CAPE_SLOTS];   // slot table read when the session was opened
    uint8_t last_status;             // last "0e 17 0d" status byte
    unsigned tx_packets;             // packets handed to io (attempts)
    unsigned rx_packets;             // replies received
    cape_progress progress;          // state of cape_session_progress_begin/step/beat (cape_progress.h); reported only through
                                      // io.progress (a save, the read of one slot, a check of hwslot)
    // Packets beyond the read-only list. NULL for a session opened with cape_session_open(), which can therefore never send one;
    // cape_wsession_open() (cape_write.h) sets them, and is the only thing that does.
    int (*extra_allowed)(const struct cape_session* s, const uint8_t* pkt);
    void* extra;                     // the owner of extra_allowed's state
} cape_session;

// 1 if the 64-byte packet is one of the read-only commands, 0 otherwise. The list, all with zero padding:
//   0e 01 00                         identification
//   0e 17 01 00 | 0e 17 04 00 | 0e 17 0d 00 | 0e 17 03 01
//   07 17 0c 0N                      select slot N (0..2) for file operations; does not change the active slot
//   07 17 07 00 <name>               open a file for reading (name: see cape_name_valid)
//   07 17 0a 00                      fill the read buffer with the next 300 bytes
//   07 17 08 00                      close the open file
//   ff NN LL 00                      read chunk NN (1..5) of LL (1..60) bytes
int cape_pkt_allowed(const uint8_t* pkt);

// 1 if name is a file name we may ask the keyboard to open: 1 to CAPE_NAME_MAX characters, the first a
// letter or digit, the rest letters, digits, '.', '_' or '-'.
int cape_name_valid(const char* name);

// The only two ways a packet leaves a session. They refuse (CAPE_E_FW, CAPE_E_BROKEN) when the session is not
// usable and CAPE_E_FORBIDDEN, ending the session, for a packet that is not on the list. cape_tx sends a
// packet that gets no reply, cape_txrx one that does. Exposed so that the tests can prove the refusals.
int cape_tx(cape_session* s, const uint8_t* pkt);
int cape_txrx(cape_session* s, const uint8_t* pkt, uint8_t* reply);
// cape_tx for a packet that continues a burst (see cape_io.send_cont): the same list, the same counters and the same refusals,
// and the packet leaves through io.send_cont (io.send if there is none). Only the writer of files uses it.
int cape_tx_cont(cape_session* s, const uint8_t* pkt);
// Passes p on to io.phase, if there is one (see cape_io.phase); NULL either way, no effect. Only cape_hwsave_run() calls it.
void cape_session_phase(cape_session* s, cape_phase p);

// Enters phase p in s->progress (cape_progress_begin(), cape_progress.h), expecting "expected" files in it, and reports it
// through io.progress, if there is one. Only cape_hwsave_run() calls it, at the same three transitions as cape_session_phase().
void cape_session_progress_begin(cape_session* s, cape_phase p, unsigned expected);
// The same for a session with one phase only (cape_progress_begin_alone()): the read of one slot, a check.
void cape_session_progress_begin_alone(cape_session* s, cape_phase p, unsigned expected);
// One file done in the current band (cape_progress_step()), reported through io.progress, if there is one. Called by
// cape_read_file(), below, for every file it reads (which covers both the read before a write and the verify after one), and by
// cape_write_slot() (cape_write.h), once for its erase-and-identity preamble and once for every file it writes.
void cape_session_progress_step(cape_session* s);
// A better count of the files of the current phase (cape_progress_expect()); the next step reports it.
void cape_session_progress_expect(cape_session* s, unsigned expected);
// Repeats the current progress value unchanged, through io.progress, if there is one: a heartbeat for a status poll that has not
// resolved yet (cape_wait_status(), below), so that a long, legitimate wait for the flash is not silence. Never itself advances
// what cape_session_progress_step() would report next.
void cape_session_progress_beat(cape_session* s);

// Opens a session: identification and firmware check ("0e 01": CAPE_E_FW and no further packet if the
// keyboard is not a tested one), behaviour check ("0e 17 01" must report 300 and "0e 17 04" a well-formed
// slot table: CAPE_E_PROTO), then one "07 17 08" that closes a file a previous session may have left open
// (its outcome is not checked). Exactly four packets on success. Any failure leaves the session unusable.
int cape_session_open(cape_session* s, const cape_io* io);

// Reads the status of the last operation ("0e 17 0d", byte 4) until it is 0 (CAPE_OK). 4 is CAPE_E_NOTFOUND if allow_notfound,
// otherwise an error, as 0d is (CAPE_E_STATUS). Any other value is not documented: it is read again, up to polls times, interval_ms
// apart (io.sleep_ms), and then it is CAPE_E_TIMEOUT. The session is not ended by a status that is an error: the caller decides.
int cape_wait_status(cape_session* s, int allow_notfound, unsigned polls, unsigned interval_ms);

// Ends the session after a failure: a file that may be open gets one close command (its outcome does not matter) and nothing else
// is ever sent. Returns err.
int cape_session_fail(cape_session* s, int err);

// Reads a whole file of slot 0..2 into buf and stores its size in *len. Returns CAPE_OK, CAPE_E_NOTFOUND
// (the session stays usable), CAPE_E_CAP (the file is larger than cap, *len has its size, the session stays
// usable), CAPE_E_RANGE (bad slot or name, nothing sent), or a failure that ends the session.
int cape_read_file(cape_session* s, unsigned slot, const char* name, uint8_t* buf, size_t cap, size_t* len);

// Ends the session. Closes a file that is still open. Never fails.
void cape_session_close(cape_session* s);

#endif  // CAPE_H
