#ifndef HIDRAW_IO_H
#define HIDRAW_IO_H

// The K95 RGB Platinum through Linux hidraw, for k95p-hwslot-tool.
// Commands go out on interface 1 and replies come on any interface; a "0e" or "ff" packet waits up to 5 s for its reply and is
// never sent again; between packets the pause of the daemon: 1 ms while reading, 11 ms while writing (iCUE's pace), none before the
// packets that continue a burst. Refuses to open when a ckb-next daemon is running or more than one K95P is attached.

#include "cape.h"

#include <poll.h>
#include <stdio.h>

#define HIDRAW_MAX_NODES 16

typedef struct {
    int out;                              // interface 1, written
    struct pollfd in[HIDRAW_MAX_NODES];   // every interface, read
    nfds_t count;
    int tested;                           // the identification said a tested firmware (cape_fw)
    int layout;                           // the physical layout it said (byte 23 + 1, CAPE_PROFILE_LAYOUT_*), 0 before it
    unsigned pause_ms;                    // before every packet but those of a burst: 1 to read, 11 to write
    FILE* transcript;                     // TX and RX lines, or NULL
    unsigned packets;
} hidraw_io;

// Opens the keyboard. Returns 0, or -1 with the reason in why.
int hidraw_open(hidraw_io* h, char* why, size_t cap);
void hidraw_close(hidraw_io* h);
// The cape_io of an open keyboard (its phase callback sets the pause: write 11 ms, read and verify 1 ms)
cape_io hidraw_cape_io(hidraw_io* h);
// Sends a packet outside a session (the change of mode around a write, the return to hardware mode). 0 or -1.
int hidraw_send(hidraw_io* h, const uint8_t pkt[CAPE_PKT_SIZE]);
// Writes a line of 64 bytes as hex to the transcript (dir "TX" or "RX")
void hidraw_log(FILE* f, const char* dir, const uint8_t pkt[CAPE_PKT_SIZE]);

#endif
