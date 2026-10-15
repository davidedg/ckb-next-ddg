#ifndef PROFILE_CAPE_H
#define PROFILE_CAPE_H

#include "includes.h"
#include "device.h"
#include "cape_slot.h"

// K95 RGB Platinum on-board profiles (see cape.h). Firmware access is gated by cape_device_start().
// The protocol and the file formats are described in the K95 RGB Platinum pages of https://github.com/davidedg/corsair-protocol
// (devices/k95p.md, formats/cape/); the comments of this code refer to them as "corsair-protocol <path>".
// Fixed pause before each packet of a read of the on-board profiles (hwload, check, and the read/verify parts of a save):
// 1 ms, which worked on the keyboard. The write and its status polls use iCUE's 11 ms instead.
#define CAPE_READ_PACING_NS 1000000L

// Decides whether the on-board profile code may talk to this device: only if the daemon was started with
// --enable-experimental and the identification that getfwversion() has just read is a tested firmware. Sets
// kb->cape_ok and logs the verdict. It sends nothing to the keyboard. Call it for every device (it clears cape_ok
// for those that are not a K95 RGB Platinum). Every way to the on-board profiles checks cape_ok first.
void cape_device_start(usbdevice* kb);

// hwload and hwsave of the K95 RGB Platinum (USES_CAPE_FS): profile_keyboard.c hands them over, with the arguments
// of the legacy handlers, and none of the legacy commands is ever sent to this keyboard.
//
// hwload reads the name and the identity of the three slots (cape_hwload.h) into kb->hw, with CAPE_READ_PACING_NS between packets,
// and makes the identity of the profile as a whole from the serial number (it does not exist on the keyboard). It reads PROFILE.I
// of each slot only, as iCUE does: the bindings and the lighting of a slot are read when it is asked for (cmd_hwslot_cape, read:).
// In hardware mode the lighting of the active slot is restarted after the read (07 04 01). It returns non-zero only if the keyboard cannot
// be talked to at all (a transport failure, for which a USB reset makes sense): every other failure is logged and
// returns 0, so that the daemon does not reset the keyboard and try again. If there is no hardware profile yet,
// the feature "hwload" is then withdrawn. Nothing is sent without kb->cape_ok.
// "apply" is ignored: there is nothing to apply to the daemon's own profile. A load that succeeds also drops the copies of slots
// whose save failed (cape_hwsave.h): what the keyboard holds is now what the daemon knows.
int cmd_hwload_cape(usbdevice* kb, usbmode* dummy1, int dummy2, int apply, const char* dummy3);

// hwsave of a mode of this keyboard is refused (cape_hwslot.h, 6): it would save the colours alone and write no PROFILE.ZIP. The
// answer, on the notification node of the command, is "hwsave fail stage=prepare file=- slot=unchanged kept=no err=<why>", and nothing
// is sent. It returns 0: a non-zero value makes command.c reset the keyboard and run the command again.
int cmd_hwsave_cape(usbdevice* kb, usbmode* mode, int nnumber, int dummy3, const char* dummy4);

// "get :hwsavecheck" of this keyboard is refused for the same reason: "hwsavecheck error err=<why>". Called with imutex held.
void cmd_get_hwsavecheck_cape(usbdevice* kb, usbmode* mode, int nnumber);

// Frees what the daemon keeps of the on-board profiles for this keyboard (freeprofile()). Safe to call twice.
void cape_device_free(usbdevice* kb);

// The daemon's copy of slot 0..2 (what the record of get :hwslot is made of), and its generation, which changes whenever the
// copy is replaced (a load, the read of the slot on demand, a save of that slot written and read back). After the attach a slot in use
// has only its PROFILE.I (whole = 0: hwslot read: reads the rest). NULL if nothing was read yet. The image belongs to the daemon and is
// valid until the next load, read or save.
const cape_slot_image* cape_device_image(usbdevice* kb, unsigned slot, unsigned* gen);

// hwslot (the specification is the comment of cape_hwslot.h). One word of "@<nnumber> hwslot ...": gathered into the
// preparation of a save; check: answers "mode <m> hwsavecheck ..." and save: saves ("mode <m> hwsave ..." and its progress lines), both on
// node nnumber only. A save or an erase made while the keyboard is in hardware mode erases the profile of the modes and switches to
// software mode for it, and back: then it returns 1, and the caller's pointers into the profile are gone. read:<m> reads a slot from its
// PROFILE.ZIP ("mode <m> hwread ..." on node nnumber, section 3); erase:<m>:<base> erases it ("mode <m> hwerase ...", section 5).
// Returns 0 otherwise. Call it without imutex.
int cmd_hwslot_cape(usbdevice* kb, int nnumber, const char* word);
// The random bytes of what a save makes new (cape_hwslot.h, section 2): the system's; a test can put its own. 0, or -1 if there are none
extern int (*cape_device_random)(uint8_t* out, size_t n);
// get :hwslot:<m>:<p> (called as every get is, with imutex held): page p of the record of slot m, from the daemon's copy (no USB)
void cmd_get_hwbind_cape(usbdevice* kb, int nnumber, const char* setting);
// Notification node closed: the preparations it owned end
void cape_node_closed(usbdevice* kb, int node);

#endif  // PROFILE_CAPE_H
