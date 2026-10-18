# k95p-hwslot-tool

A standalone command-line tool for the three hardware slots (M1, M2, M3) of the Corsair K95 RGB Platinum, with
`k95p-restore-slot.sh`, which writes one slot back from the local copy iCUE keeps of it.

iCUE keeps a copy of every hardware profile it has read from or written to the keyboard, but it has no way to write such
a copy back to a slot. If a slot was erased or overwritten, the copy is often the only record of what it held. This
directory turns that copy into the files iCUE itself would have written to the slot (the profile document `PROFILE.ZIP`
included, so iCUE still recognises the slot), rehearses the write on a simulated keyboard, and only then writes the slot.

The tool uses the K95 Platinum code of the ckb-next daemon in this checkout (`src/daemon/cape*.c`). It is not built with
ckb-next and no binaries are distributed: build it yourself.

**Experimental. `go` writes the flash of your keyboard.** Read this whole page first.

## Scope

- Only the K95 RGB Platinum, USB `1b1c:1b2d`, with firmware 3.29 and bootloader 3.03. Any other firmware is refused
  before anything is written (`src/daemon/cape_fw.c`). Do not update or downgrade the firmware for this tool.
- No other device is opened: the tool selects only `HID_ID 0003:1B1C:1B2D` and refuses if more than one is attached.
- The profile documents follow iCUE 5.51.40 (`--compat icue-5.51.40`). The converter refuses a local copy with a
  class or class version it does not know; nothing is guessed.
- Linux only (hidraw).

## Requirements

- A C compiler and the zlib headers (`zlib-devel`, `zlib1g-dev` or `zlib`, depending on the distribution).
- Python 3.9 or later.
- `lsusb` (usbutils), `sudo`, `sha256sum`.
- For `go`, by default: `dumpcap` (Wireshark) and the `usbmon` kernel module, to record the USB traffic of the write.
  `K95P_RESTORE_CAPTURE=0` skips the capture.

## Build

```sh
cd tools/k95p-hwslot-tool
./build.sh                 # prints the path of the binary, ./build/k95p-hwslot-tool
WERROR=1 ./build.sh        # warnings as errors
```

`k95p-restore-slot.sh` builds its own copy in each directory it makes, so a manual build is only needed to use the tool
directly.

## Before you start

- Stop ckb-next completely: quit the GUI and stop the daemon (`sudo systemctl stop ckb-next-daemon`). The tool refuses to
  talk to the keyboard while a ckb-next daemon runs, and the script refuses while the GUI or the service is running.
- Close iCUE, if the keyboard is shared with a Windows machine or virtual machine, and attach the keyboard to Linux.
- Find the local copy. On Windows it is
  `%APPDATA%\Corsair\CUE5\hw_profiles\{GUID}.cueprofiledata`, one file per hardware profile. Copy it to Linux; never edit
  the original.

## Restore a slot

Every step writes into a new directory, which must not exist yet. Nothing is overwritten and nothing is reused.

### 1. Dump the keyboard (read-only)

```sh
./k95p-restore-slot.sh dump ~/k95p/dump-1
```

This sends only the read commands of the slot file system and a final `07 04 01` (back to hardware mode), as root
through sudo. The image of the three slots goes to `~/k95p/dump-1/dump`. The lighting can blink briefly while a slot that
is not the active one is read. Keep this dump: it is also your backup of the current slots.

### 2. Prepare the write (offline)

```sh
./k95p-restore-slot.sh prepare COPY.cueprofiledata SLOT "NAME" ~/k95p/dump-1/dump ~/k95p/work-1
```

- `SLOT` is 0, 1 or 2 (M1, M2, M3) and must be the slot the copy belongs to (its `slotIndex`).
- `NAME` is the profile name stored in the slot, usually the one iCUE shows.

`prepare` converts the copy (`icue-local-to-xml.py`), compiles the slot from it (`k95p-hwslot-tool zip --new`) and checks
that the files and the document agree (`check`). It then rehearses the write on a simulated keyboard loaded with the dump
and reads the result back. `work-1/prediction.txt` lists the files, the packets and the new cookie of the slot, and confirms
that the other two slots stay byte for byte as they are. Everything is sealed in `work-1/SHA256SUMS`.

Read `prediction.txt` before going on.

### 3. Write the slot

```sh
K95P_RESTORE_GO=1 ./k95p-restore-slot.sh go ~/k95p/work-1 ~/k95p/run-1
```

`go` refuses without `K95P_RESTORE_GO=1`, if the sealed directory changed, if ckb-next is running, or if there is not
exactly one K95 Platinum. It then:

1. starts `dumpcap` on the keyboard's USB bus, before any packet is sent;
2. reads the slot table again and stops if the slot is not what the dump found (nothing written);
3. switches to software mode as the ckb-next daemon does, writes the slot, reads every file back and returns to hardware
   mode;
4. dumps the keyboard again, read-only, into `run-1/after`;
5. stops the capture and compares the packets sent and the files read back with the prediction.

A write is never repeated automatically. If anything fails, `run-1` keeps the transcript, the logs and the capture.
Start again from a new dump, never from an old one.

Select the restored slot with the profile button of the keyboard to use it.

## The tool

```
k95p-hwslot-tool [--hidraw | --sim IMAGE [--sim-out DIR]] [--transcript F] [--layout N] COMMAND ...
  dump [--slot N] DIR                    read the slots into a new directory (read-only)
  zip IMAGE [--slot N] [--xml F] -o DIR  build the files of a slot, PROFILE.ZIP included
  check IMAGE [--slot N]                 the files of a slot against its PROFILE.ZIP
  xml ZIP                                the XML document of a PROFILE.ZIP
  diff A B [--slot N] [--uuid-map]       two images, file by file, PROFILE.ZIP by model
  convert --slot N IMAGE --base ... [--go]
                                         write one slot of an image; without --go it only reads
```

An image is a directory of `slotN_<file>_<read|write>_NNN.bin` files, with `slot-table.txt` and `layout.txt`. `--sim`
runs a command on a simulated keyboard loaded from an image, which is how `prepare` makes its prediction. The header of
`k95p-hwslot-tool.c` documents every option.

## Files

| File | Purpose |
| --- | --- |
| `k95p-hwslot-tool.c` | the tool |
| `hidraw_io.c`, `hidraw_io.h` | the keyboard through Linux hidraw |
| `cape_sim.c`, `cape_sim.h` | the simulated keyboard of the rehearsal |
| `icue-local-to-xml.py` | iCUE local copy to the `PROFILE.ZIP` document |
| `profile_xml_codec.py` | reader and writer of that document, used by the converter |
| `k95p-restore-slot.sh` | the dump, prepare and go steps |
| `build.sh` | the build |

Same licence as ckb-next (GPL-2.0).
