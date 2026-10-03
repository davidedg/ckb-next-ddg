#ifndef CAPE_SLOT_H
#define CAPE_SLOT_H

/*
 * A profile slot of the Corsair K95 RGB Platinum (USB 1b1c:1b2d) as a set of files that can be rewritten: what hwsave needs
 * to change the lighting and the name of a slot and to keep everything else in it as it is (corsair-protocol devices/k95p.md#implementation-notes).
 *
 * Writing a slot in iCUE's way empties it and writes every file again (there is no file-level write: corsair-protocol devices/k95p.md#write-a-slot), so
 * whatever the writer does not know how to say is lost unless it is kept byte for byte: the key remaps (PROFILE.MAP), the
 * macro table (PROFILE.DAT, which also holds the Win Lock options), the macro files it names (M000, M001...) and the rest of
 * PROFILE.I (the indicator colours, a name longer than the daemon's, whatever else is in it). cape_slot_read() reads them and
 * cape_slot_plan_build() makes the new set from them and from the picture the user drew: the lighting files are new, PROFILE.I
 * has the new name (if it changed), the next cookie and the same GUID and indicator colours, and the others are the ones that
 * were read, and so are the lighting files where the picture is the one the slot's lighting already makes: a rename does not flatten
 * opacities and overlaps, and does not turn a slot of eight colours into a refusal. PROFILE.ZIP, iCUE's own copy of the profile, is not part of it: it is neither read nor written (the project
 * does not coexist with iCUE), so a slot written here has none.
 *
 * Nothing is rewritten that cannot be kept whole: a slot whose PROFILE.DAT is not understood cannot say which macro files it
 * has (the file system has no directory listing), one whose DAT names a file that does not exist would leave a dangling
 * reference, and so on: cape_slot_read() marks it "refused" with the reason, and the plan for it is a refusal.
 *
 * This module reads through the read-only transport (cape.h) and sends nothing else; the planning is pure. Verified on
 * firmware 3.29, bootloader 3.03.
 */

#include "cape.h"
#include "cape_binding.h"
#include "cape_light.h"
#include "cape_slotfiles.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_SLOT_WHY         160
// PROFILE.I is 268 bytes: a file larger than this is not one, and the slot is refused
#define CAPE_SLOT_INFO_CAP    4096
// The names of the modes in the GUI have this many UTF-16 units (the daemon's MD_NAME_LEN), fewer than iCUE's
#define CAPE_SLOT_GUI_NAME_UNITS  16

// A slot as read: what is in it that a rewrite has to keep, and what its lighting is now. It owns memory (the files, the model of the
// bindings): it is freed with cape_slot_image_free() and is not to be copied by value into another that is freed too.
typedef struct {
    int present;                  // the slot table says it holds a profile; if it does not, nothing else is set
    int whole;                    // everything was read (or the slot is empty); 0: only PROFILE.I (cape_slot_read_head), the bindings
                                  // and the lighting are not known (bind is EMPTY and light UNKNOWN only because they were not read)
    int refused;                  // it does, and it cannot be rewritten at all (PROFILE.I is missing or not valid); reason says why.
                                  // The bindings are judged on their own (bind), and so is the lighting (light.complete)
    char reason[CAPE_SLOT_WHY];
    cape_slotid id;               // its entry of the slot table
    cape_info info;               // PROFILE.I as parsed (valid if info_ok)
    int info_ok;
    uint8_t winlock;              // the Win Lock options of PROFILE.DAT (valid if bind.winlock_read)
    cape_slot_files files;        // as read, in this order: PROFILE.I, PROFILE.DAT, PROFILE.MAP, the macro files in the order of the table
    cape_binding_model bind;      // the bindings: OK (a model), RAW (their files can be kept, not edited), BROKEN (not even kept)
    cape_light_slot light;        // the lighting as it is now (cape_light_read_all), the indicator colours of PROFILE.I included
    cape_slot_files light_files;  // its files as read (lghtcnt.cnt, lght_NN.d/.k/.r), whatever the lighting is
    unsigned warnings;            // things that looked wrong and were kept: a GUID that differs from the table, a macro of another size
    unsigned packets;             // packets the reading took
} cape_slot_image;

void cape_slot_image_free(cape_slot_image* img);

// Reads slot 0..2 of an open session: everything that can be read of it, whatever is wrong with part of it. An empty slot is present
// = 0 and costs no packet. What is wrong with a slot is not an error but a state (refused, bind, light); only a failure of the session is
// (the negative CAPE_E_* code, and then the image is not to be used). The image must be freed with cape_slot_image_free() either way.
int cape_slot_read(cape_session* s, unsigned slot, cape_slot_image* out);
// Only PROFILE.I of slot 0..2 (its identity, name and indicator colours, or refused), as iCUE reads the slots when it starts (corsair-protocol
// devices/k95p.md#behaviour): 12 packets for a slot in use, none for an empty one. whole is 0 for a slot in use (1 for an empty one: there is
// nothing else to read). Returns as cape_slot_read().
int cape_slot_read_head(cape_session* s, unsigned slot, cape_slot_image* out);
// The same as cape_slot_read() from files in memory (a save that was written and read back), with the entry of the slot table they belong to (an empty
// entry gives an empty slot). CAPE_OK, CAPE_E_ARG, or CAPE_E_CAP if memory ran out.
int cape_slot_image_from_files(const cape_slot_files* files, const cape_slotid* id, cape_slot_image* out);

// What the user wants the slot to be
typedef struct {
    const uint8_t* r;             // the colour of each of the leds LEDs of the daemon's keymap (cape_light_pack)
    const uint8_t* g;
    const uint8_t* b;
    size_t leds;
    const uint16_t* name;         // the name of the mode, UTF-16 (NULL or no units: keep the slot's)
    size_t name_units;
    const uint8_t* guid;          // 16 bytes, the GUID of the profile made in an empty slot (ignored for one that has a GUID)
    const uint8_t (*order)[3];    // NULL, or the colours of the layers in the order to write them (cape_light_pack_ordered): what the
    size_t norder;                // tests that compare the files with iCUE's own saves use; nobody else has a reason to
    int force;                    // write the slot even if it already looks like the picture and the name: the verdict is then WRITE and
                                  // the files are written again, the lighting ones as they are (the user asked for it to be rewritten)
    // hwslot (all zero for hwsave: the bindings kept as they are, the lighting from the picture, no base)
    const cape_binding_model* bindings;   // the bindings to write, an OK model (the Win Lock options with them); NULL: keep the slot's
                                          // PROFILE.MAP, PROFILE.DAT and macro files as they are
    int recreate;                 // the bindings replace bindings of the slot that are not a model (RAW, BROKEN): required for those, and
                                  // refused for bindings that are a model (those are edited)
    int light_keep;               // keep the lighting files as they are (r, g, b are not used): refused if they were not all read
    const cape_slotid* base;      // the entry of the slot table the edit was made on (all zeros: an empty slot); a slot that is not that
                                  // one any more is refused before anything is written. NULL: not checked (hwsave)
    // The Performance settings (zero when an edit has none: the slot's are kept, iCUE's defaults for a new profile)
    int has_winlock;              // winlock are the Win Lock options to write: every PROFILE.DAT written has them in its byte 1, also one
    uint8_t winlock;              // that is kept as it is (the bindings' files of the slot, RAW ones too)
    int has_ind;                  // ind are the indicator colours to write into PROFILE.I (profile, brightness, lock on, lock off: RGB)
    uint8_t ind[CAPE_IND_COUNT * 3];
} cape_slot_edit;

typedef enum {
    CAPE_SLOT_REFUSED = 0,
    CAPE_SLOT_SAME,               // the slot already looks like that (same picture, same name): nothing needs writing
    CAPE_SLOT_WRITE,              // the slot is in use and would change
    CAPE_SLOT_NEW                 // the slot is empty: a profile is made in it
} cape_slot_verdict;

#define CAPE_REPLACES_EFFECTS  0x01   // the lighting has effect layers (predefined or custom), which a write removes
#define CAPE_REPLACES_UNKNOWN  0x02   // the lighting files were not understood, and a write replaces them

typedef struct {
    cape_slot_verdict verdict;
    char reason[CAPE_SLOT_WHY];       // for CAPE_SLOT_REFUSED
    unsigned colours;                 // the colours other than black in the picture (cape_light_pack)
    unsigned layers;                  // the layers that will be written
    unsigned ignored;                 // LEDs with a colour that no layer can hold (cape_light_pack)
    unsigned replaces;                // CAPE_REPLACES_*
    unsigned effect_layers;           // the predefined and custom effect layers now in the slot
    unsigned buttons_changed;         // CAPE_BTN_*: the buttons painted differently from their indicator colours in PROFILE.I
    int name_changed;
    int keeps_lighting;               // the lighting files are kept as they are: the picture is the slot's own (opacities, overlaps, more
                                      // colours than a profile is made of stay), or the edit asked to keep them (light_keep)
    int bindings_changed;             // the bindings to write do something else than the slot's (hwslot)
    int winlock_changed;              // the Win Lock options to write are not the slot's (Performance)
    int indicators_changed;           // the indicator colours to write are not the slot's (Performance)
    cape_slotid id;                   // the identity to write (WRITE and NEW)
} cape_slot_plan;

// Works out what to do with a slot read as old (which may be a refusal or an empty slot) to make it what edit says. The verdict is
// SAME only if the name, the bindings, the Win Lock options, the indicator colours and the lighting are all the slot's. Bindings that are the slot's are written back as the files
// that were read; other ones are built as iCUE builds them (cape_binding_build), after cape_binding_check against the slot's. For
// WRITE and NEW the files to write are put in out, in the order iCUE writes them: the macro files, PROFILE.DAT, PROFILE.I,
// PROFILE.MAP, then the lighting, lghtcnt.cnt last (out must have been zeroed or used before: what it holds is freed first;
// NULL is allowed for a plan that is only a check). Returns CAPE_OK, also for a refusal, CAPE_E_ARG for bad arguments.
int cape_slot_plan_build(const cape_slot_image* old, const cape_slot_edit* edit, cape_slot_plan* plan, cape_slot_files* out);

const char* cape_slot_verdict_name(cape_slot_verdict v);

#endif  // CAPE_SLOT_H
