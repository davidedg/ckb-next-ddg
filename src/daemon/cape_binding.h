#ifndef CAPE_BINDING_H
#define CAPE_BINDING_H

/*
 * The hardware bindings of a K95 Platinum slot (PROFILE.MAP, PROFILE.DAT and the macro files M###) as a model that can be
 * compared, edited and written back (hwslot; corsair-protocol formats/cape/profile-map.md, profile-dat.md, macro.md).
 *
 * The import is strict. A slot whose files the model says exactly is OK: building its files again gives the same bytes (file
 * names aside, which are numbered again as iCUE numbers them). Anything else is not dropped and not guessed:
 *  - RAW: every file is there, but something in them is outside what the model holds (a MAP flag other than 0x80, a key in
 *    both files or twice, a run type or subtype iCUE does not write, an event nobody has decoded, a byte that should be zero,
 *    a macro file of another size than the DAT says). The files can be kept byte for byte, as hwsave keeps them, but not
 *    edited: only rebuilt from scratch.
 *  - BROKEN: the files cannot even be kept (a DAT or MAP that does not parse, a DAT that names a file that is not there or
 *    is not a macro file). Only a rebuild from scratch can write the slot again.
 *
 * Values that are in the format and that iCUE offers, but that iCUE has not been seen writing, are imported and kept but not
 * editable (CAPE_BIND_F_LOCKED); combinations that iCUE does not write are marked CAPE_BIND_F_NON_ICUE.
 * The model owns its events: it must be freed with cape_binding_free(), and copied with cape_binding_copy().
 *
 * Pure code: nothing here talks to the device. Verified on firmware 3.29, bootloader 3.03.
 */

#include "cape_slotfiles.h"

#include <stddef.h>
#include <stdint.h>

// The K95P file formats use the first 152 entries of the daemon keymap as key indices
#define CAPE_BIND_K95_KEYS       152
// A remap may also send a mouse button: mouse1..mouse5
#define CAPE_BIND_MOUSE_FIRST    0xc8
#define CAPE_BIND_MOUSE_LAST     0xcc
// iCUE writes the remaps in ascending order of source, except the profile key, which comes last (icue-save-multi2)
#define CAPE_BIND_KEY_PROFSWITCH 0x46
#define CAPE_BIND_WHY            160

// Limit of what is generated or edited. What is kept as the slot had it is never limited by it.
// iCUE's largest macro file: a Text of 1024 characters (the limit of its field) that all need Shift, 16 390 bytes in 5 flash
// sectors. There is no limit on the bytes of a slot: the writer checks the free space of the flash.
#define CAPE_BIND_EDIT_EVENTS_MAX  8191
// iCUE's GUI for a hardware profile (corsair-protocol formats/cape/macro.md): a delay of a Macro is at most 4095 ms, a Macro at most 256 rows (its
// editor shows every event but the delay of 0 its writer puts between two key events), the delay between the characters of a Text
// at most 99999 ms, 'Imitate holding key' on press 0.1 to 16 777.2 s
#define CAPE_BIND_MACRO_DELAY_MAX  4095
#define CAPE_BIND_MACRO_ROWS_MAX   256
#define CAPE_BIND_TEXT_DELAY_MAX   99999
#define CAPE_BIND_IMITATE_MIN_MS   100
#define CAPE_BIND_IMITATE_MAX_MS   16777200
// The keys iCUE offers: its Keyboard picker, its media keys, the 9 Language keys it writes on a key
#define CAPE_BIND_ICUE_KEYBOARD    0x01
#define CAPE_BIND_ICUE_MEDIA       0x02
#define CAPE_BIND_ICUE_LANGUAGE    0x04

typedef enum {
    CAPE_BIND_EMPTY = 0,   // no profile in the slot
    CAPE_BIND_OK,
    CAPE_BIND_RAW,
    CAPE_BIND_BROKEN
} cape_binding_state;

typedef enum {
    CAPE_BIND_NATIVE = 0,
    CAPE_BIND_REMAP,
    CAPE_BIND_MACRO
} cape_binding_kind;

#define CAPE_BIND_F_SHARED    0x01   // its macro file is named by another entry of the DAT too (a new write gives each its own copy)
#define CAPE_BIND_F_NON_ICUE  0x02   // a combination of subtype, start and run that iCUE does not write
#define CAPE_BIND_F_LOCKED    0x04   // values iCUE has not been seen writing: kept, not editable

typedef struct {
    cape_binding_kind kind;
    uint8_t dst;              // REMAP: the key index (< CAPE_BIND_K95_KEYS) or a mouse button
    uint8_t subtype;          // MACRO: CAPE_MACRO_MACRO, CAPE_MACRO_SHORTCUT or CAPE_MACRO_TEXT
    uint8_t start;            // MACRO: 0x00 on press, 0x11 on release
    uint8_t run;              // MACRO: 1 uninterrupted, 3 while pressed, 4 toggle
    uint8_t repeat;           // MACRO
    cape_event* events;       // MACRO: owned
    unsigned nevents;
    char file[5];             // MACRO: the name of its file in the slot it was read from ("" for a new one): shown, not compared
    unsigned flags;           // CAPE_BIND_F_*, worked out from the values: not compared
} cape_binding_action;

typedef struct {
    cape_binding_state state;
    char reason[CAPE_BIND_WHY];         // RAW and BROKEN: why
    uint8_t winlock;                    // the Win Lock options (byte 1 of PROFILE.DAT)
    int winlock_read;                   // PROFILE.DAT was read, so winlock is the slot's (also for RAW and some BROKEN)
    cape_binding_action key[CAPE_BIND_K95_KEYS];
    unsigned remaps;
    unsigned macros;
    unsigned files;                     // the distinct macro files of the slot it was imported from (shared ones count once)
} cape_binding_model;

// Imports the bindings of the files of a slot (PROFILE.MAP, PROFILE.DAT and the macro files they name; cape_slot_read() does it for
// the slot it reads). Returns CAPE_OK (whatever the state), CAPE_E_ARG, or CAPE_E_CAP if memory ran out (out is then EMPTY).
int cape_binding_import_files(const cape_slot_files* files, cape_binding_model* out);
// A model that says the bindings cannot be kept, for a reason found before the files could be imported (a file that could not be
// read). winlock_read says whether PROFILE.DAT was read, and winlock is then its options.
void cape_binding_broken(cape_binding_model* m, const char* why, int winlock_read, uint8_t winlock);

// An OK model with no bindings and iCUE's default Win Lock options: what a slot rebuilt from scratch starts from.
void cape_binding_init(cape_binding_model* m);
void cape_binding_free(cape_binding_model* m);
// A deep copy (dst is overwritten, not freed). Returns CAPE_OK, CAPE_E_ARG or CAPE_E_CAP if memory ran out (dst is then EMPTY).
int cape_binding_copy(cape_binding_model* dst, const cape_binding_model* src);

// Edits of an OK model. key < CAPE_BIND_K95_KEYS. The values are checked by cape_binding_check(), not here.
int cape_binding_set_native(cape_binding_model* m, unsigned key);
int cape_binding_set_remap(cape_binding_model* m, unsigned key, uint8_t dst);
// Copies the events. Returns CAPE_OK, CAPE_E_ARG or CAPE_E_CAP if memory ran out (the key is then unchanged).
int cape_binding_set_macro(cape_binding_model* m, unsigned key, uint8_t subtype, uint8_t start, uint8_t run, uint8_t repeat,
                           const cape_event* events, unsigned nevents);

// Two actions, or two models, that make the keyboard do the same: the kind, the destination, subtype, start, run, repeat and
// every event. The names of the files, their order and the flags do not count, and neither do the Win Lock options: they are not
// part of the bindings (a save writes them into whatever PROFILE.DAT it writes, cape_slot_edit.winlock).
int cape_binding_action_equal(const cape_binding_action* a, const cape_binding_action* b);
int cape_binding_equal(const cape_binding_model* a, const cape_binding_model* b);

// Whether edit, an OK model, can be written in place of base (the slot as read, any state; NULL = an empty slot). An action equal
// to base's action for the same key is kept as it is and is not checked; any other must be something iCUE writes, 1 to
// CAPE_BIND_EDIT_EVENTS_MAX events whose presses and releases pair up:
//  - a remap to a key iCUE offers (CAPE_BIND_ICUE_*) or a mouse button;
//  - a Macro on press (uninterrupted, while pressed or toggle) or on release (uninterrupted), repeat 1, its delays of 2 bytes and at
//    most CAPE_BIND_MACRO_DELAY_MAX ms or random with min <= max, at most CAPE_BIND_MACRO_ROWS_MAX rows as iCUE can write them
//    (cape_binding_macro_rows);
//  - a Shortcut on press and uninterrupted, repeat 1, in the form of iCUE's Keystroke (up to three of the left Win, Ctrl, Alt, Shift
//    in this order, then one key of iCUE's keyboard); or 'Imitate holding key' (a key of iCUE's keyboard pressed, a delay,
//    released): on press, run 1 repeat 1, CAPE_BIND_IMITATE_MIN_MS to CAPE_BIND_IMITATE_MAX_MS in steps of 100 ms or c0 ff ff ff
//    (iCUE's clamp), or its toggle mode, run 84 repeat 0, a delay of 0;
//  - a Text on press and uninterrupted, repeat 1, its delays constant in iCUE's form and at most CAPE_BIND_TEXT_DELAY_MAX ms.
// The shape of the characters of a Text, and the delay of 0 iCUE puts between two key events of a Macro, are the GUI's to make: not
// checked here. No limit on the whole slot: a macro file per key always fits (CAPE_SLOT_MACROS_MAX), and whether the files fit in
// the flash is for the writer to say (cape_write_slot). Returns CAPE_OK, CAPE_E_ARG, or CAPE_E_RANGE with the reason in why (why
// may be NULL).
int cape_binding_check(const cape_binding_model* edit, const cape_binding_model* base, char* why, size_t whycap);

// Adds PROFILE.MAP, PROFILE.DAT and the macro files of an OK model to out, the way iCUE writes them: the DAT in ascending order of
// key, the macro files numbered M000, M001... in that order (a shared file becomes one file per key), the MAP in ascending order
// of source with the profile key last. Returns CAPE_OK, CAPE_E_ARG (not an OK model), CAPE_E_CAP (no room in out, or memory ran out),
// CAPE_E_RANGE (a file larger than the formats can say). Nothing is added on failure.
int cape_binding_build(const cape_binding_model* m, cape_slot_files* out);

const char* cape_binding_state_name(cape_binding_state s);
// The sets of iCUE's GUI a key index is in (CAPE_BIND_ICUE_*), 0 for none
unsigned cape_binding_icue_sets(uint8_t key);
// The rows of a Macro as iCUE can write them: every event but a delay of 0 between two key events (iCUE's writer adds those)
unsigned cape_binding_macro_rows(const cape_event* ev, unsigned n);

#endif
