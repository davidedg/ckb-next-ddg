#ifndef CAPE_FW_H
#define CAPE_FW_H

/*
 * Which keyboards may be accessed through the on-board profile file system.
 *
 * Everything the daemon does with the profile slots (the 0x17 file commands and the 0x15 slot commands) was
 * reverse engineered and verified on one keyboard at one firmware level. Another level may behave differently
 * (a K95 Platinum with firmware 3.08 has been seen in the wild), and a wrong file command can corrupt the
 * flash, so the daemon must refuse to touch the slots of any device that is not on the list below, for reads
 * as well as writes. There is deliberately no option to override this.
 *
 * How to add a firmware level: verify the behaviour on a real keyboard first (dump the slots, read them
 * back, write a test profile into a slot you can afford to lose), then add a row to the table in
 * cape_fw.c and rebuild.
 *
 * The identity is read from the "0e 01" identification reply: vendor and product ID, and the BCD-encoded
 * application and bootloader versions (0x0329 is 3.29). Other identification data exists (iCUE shows the
 * firmware as 3.29.32) but it is not in any field the daemon reads, so two builds of the same version cannot
 * be told apart.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t app_bcd;  // application firmware, 0x0329 is 3.29
    uint16_t bld_bcd;  // bootloader, 0x0303 is 3.03
} cape_fwinfo;

// Minimum length of the "0e 01" reply that cape_fwinfo_from_ident() needs.
#define CAPE_FW_IDENT_MIN_LEN  16

// Reads the identity from a "0e 01" reply. Returns CAPE_OK, CAPE_E_ARG for NULL pointers, CAPE_E_SIZE if the
// reply is shorter than CAPE_FW_IDENT_MIN_LEN, or CAPE_E_MAGIC if it does not start with 0e 01. Anything
// that fails must be treated as "not tested".
int cape_fwinfo_from_ident(const uint8_t* reply, size_t len, cape_fwinfo* out);

// 1 if the device is on the list of tested ones, 0 otherwise (also for NULL).
int cape_fw_is_tested(const cape_fwinfo* info);

// Writes "1b1c:1b2d fw 3.29 bld 3.03" into buf like snprintf: at most cap bytes including the terminator,
// returns the length the text has (excluding the terminator), or a negative CAPE_E_* value for NULL
// arguments.
int cape_fw_describe(const cape_fwinfo* info, char* buf, size_t cap);

// Writes the tested levels, e.g. "1b1c:1b2d fw 3.29 bld 3.03", separated by ", ". Same conventions.
int cape_fw_tested_list(char* buf, size_t cap);

#endif  // CAPE_FW_H
