#ifndef CAPE_COMPILE_H
#define CAPE_COMPILE_H

/*
 * The model of PROFILE.ZIP (cape_profile.h) and the other files of a slot of the Corsair K95 RGB Platinum: what iCUE writes into
 * PROFILE.MAP, PROFILE.DAT, the macro files, the lighting files and the indicator colours of PROFILE.I for a profile. iCUE never
 * reads those files back: at every save it makes all of them again from its model, and this module does the same.
 *
 * A model is compiled in three parts, each of which succeeds or fails on its own:
 *  - the bindings: the actions of the keys into the MAP, the DAT and the macro files (through the bindings model of
 *    cape_binding.h, whose writer gives iCUE's order and numbering), with the Win Lock options in byte 1 of the DAT;
 *  - the indicator colours: the 12 bytes of PROFILE.I from offset 253;
 *  - the lighting: enabled layers only, in reverse model order with compact indices; disabled layers stay in PROFILE.ZIP.
 *    lghtcnt.cnt counts only enabled layers, including zero when all are disabled. Static layers become lght_NN.d, .k and .r.
 *    Animated custom layers become lght_NN.d and .k; predefined layers become lght_NN.d. Every descriptor is compiled,
 *    including its sampled colour curve and origin. Cells, order and origins depend on the physical keyboard layout.
 * What iCUE writes is what is compiled, what its writer does not put in the files is left out of them (the sound and the
 * lighting of an action, its second-press options, the termination and the priority of a Macro, the brightness of a layer, the
 * side lighting...). What the files cannot hold, or a choice of iCUE's writer that is not known (a Macro that repeats, more than
 * five layers...), is CAPE_COMPILE_E_UNCOMPILABLE with the reason in why. A light of another layout than the keyboard's (the
 * ANSI backslash on an ISO keyboard) is CAPE_COMPILE_E_NOCELL.
 *
 * How iCUE turns an action into the files:
 *  - a remap to a key or a mouse button is an entry of the MAP; a Media action is a remap to its media key, of no key nothing; a
 *    remap that names neither a key nor a shortcut has no entry (the key does what it does without a profile); a shortcut comes
 *    before the key, and 'Imitate holding key' while pressed is the plain remap;
 *  - a Macro, a Text, a shortcut and 'Imitate holding key' are a macro file each (subtypes 0, 3, 1 and 1) and an entry of the DAT.
 *    Only a Macro takes when it starts and how it runs: on release starts at 0x11, while pressed runs 3, toggle 4, anything else
 *    (a long press too) on press, 1. Every other kind is on press and runs 1, but 'Imitate holding key' in its toggle mode, which
 *    runs 0x84 with a repeat of 0;
 *  - between two key events of a macro file there is a delay of 0 (80 00), never after the last one; a delay is 2 bytes below
 *    8192 ms and 4 from there (c0 and 24 bits), at most c0 ff ff ff; a random delay has each bound cut to 4095 ms, in its order;
 *    a key event of a Macro is one of each of its keys, a stroke all the presses and then all the releases;
 *  - the keys of a shortcut are pressed in the order they are listed and released in the reverse order;
 *  - a Text is typed as Latin-1 with the keys of the US layout whatever the layout of the keyboard, with Shift around the
 *    characters that need it, a line feed being Enter: a UTF-16 unit beyond U+00FF is a '?', any other character with no key (the
 *    carriage return of a Windows line end, an accented letter) is left out.
 *
 * Pure code: nothing here talks to the device or keeps state. Memory: the bindings model owns its events (cape_binding_free).
 */

#include "cape_binding.h"
#include "cape_profile.h"
#include "cape_names.h"
#include "cape_light.h"
#include "cape_lightmodel.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_COMPILE_E_UNCOMPILABLE  (-80)   // the model holds what the files of the K95P cannot, or what iCUE does not write there
#define CAPE_COMPILE_E_NOCELL        (-81)   // a light of another layout than the keyboard's

// Short description of any code of cape_profile_strerror() or of this module.
const char* cape_compile_strerror(int err);

// The bytes of PROFILE.I that hold the indicator colours: profile, brightness, Win Lock on and off, RGB each
#define CAPE_COMPILE_IND_OFFSET  253
#define CAPE_COMPILE_IND_SIZE    12

// One action, as what the files hold for the key it is on: kind REMAP (dst), MACRO (subtype, start, run, repeat and the events of
// its file, owned: free() them) or NATIVE (no entry). file is empty. Returns CAPE_OK, CAPE_COMPILE_E_UNCOMPILABLE (why says
// what), or CAPE_E_CAP if memory ran out. out is cleared on failure.
int cape_compile_action(const cape_profile_action* a, cape_binding_action* out, char* why);

// The bindings of a model: its actions and its Win Lock options, as a bindings model in state OK (cape_binding_build() makes the
// files). out is initialised here (cape_binding_free it, also on failure). Returns CAPE_OK, CAPE_COMPILE_E_UNCOMPILABLE or
// CAPE_E_CAP.
int cape_compile_bindings(const cape_profile_profile* p, cape_binding_model* out, char* why);

// The indicator colours, as the bytes of PROFILE.I from CAPE_COMPILE_IND_OFFSET (a colour the model leaves unset is iCUE's
// default: ff0000 for the profile, 00ffff for Win Lock on, ff0000 off). Returns CAPE_OK or CAPE_E_ARG.
int cape_compile_indicators(const cape_profile_profile* p, uint8_t out[CAPE_COMPILE_IND_SIZE]);

// The lighting of a model
typedef struct {
    int effects;                                  // 1: some enabled layer is animated
    unsigned layers;                              // enabled layers: what lghtcnt.cnt says, not the ZIP model's layer count
    cape_light_file file[CAPE_LIGHT_FILES_MAX];   // all files, in the order iCUE writes them
    size_t nfiles;
    unsigned count;                               // the number of files the lighting has (also with effects)
    size_t bytes;                                 // the sum of their sizes, lghtcnt.cnt included (also with effects)
} cape_compile_lighting;

// layout: the physical layout of the keyboard (CAPE_PROFILE_LAYOUT_*); a model with layers on a layout that is not one of the five is
// CAPE_COMPILE_E_UNCOMPILABLE. The five-custom or one-predefined limit includes disabled layers. Returns CAPE_OK (also with
// effects), CAPE_COMPILE_E_UNCOMPILABLE, CAPE_COMPILE_E_NOCELL or CAPE_E_ARG.
int cape_compile_lighting_build(const cape_profile_profile* p, int layout, cape_compile_lighting* out, char* why);

/*
 * Importing: the files of a slot back into a model, the other way.
 *
 * iCUE's model holds more than the files: the names and identities, the string of a Text (with the characters the files leave
 * out), which tab of iCUE an action was chosen in, the colours of the M1 to M3 buttons. An import starts from a base, the model of
 * the slot as it was (its PROFILE.ZIP), when there is one, and goes key by key:
 *  1. an action of the base on the same key that compiles to exactly what the files hold for that key is kept whole;
 *  2. otherwise the files are inverted into the action iCUE would make for them. If the base has an action of the same kind on
 *     the key, its name and identity are kept (iCUE edits an action in place), and so are its other values where they compile
 *     to the same files (what the files do not hold, the string of a Text typed again with a new delay, a longer interval of
 *     'Imitate holding key'); else
 *     the action gets a new identity and the name "<kind> N", N the first number free.
 * The actions keep the order of the base, the new ones follow by key. The lighting is the base's when the files are those its
 * layers compile to, byte for byte), else static layers
 * made from the files; the indicator colours are the base's where they compile to the same bytes. What the files do not hold
 * at all comes from the base, or is what iCUE gives a new profile.
 *
 * Every action and layer made from the files is compiled again and must give exactly the files: a macro file that is no action
 * of iCUE (a Text typed with keys its US table does not use, a Text that starts on release) is CAPE_COMPILE_E_UNCOMPILABLE, or,
 * in the converting mode, becomes a Macro of the same events (only the subtype of its file changes), if a Macro can hold them.
 * So must the files of the bindings as a whole (the order of the MAP and the DAT, the numbering of the macro files): a slot
 * laid out otherwise is refused, or taken in the converting mode (opts.relaid).
 * Layers of effects cannot be made from the files: without a base whose layers they are, the import fails.
 */

// Compact XML grouping of a Macro (M, then p/r/s plus four hex digits of key count, or d for a delay),
// or a plain remap's identity (R, two hex digits of remapGroup, colon, keyName). NULL when inapplicable.
// The result is allocated; the caller frees it. This metadata never changes executable events.
int cape_compile_shape(const cape_profile_action* action, char** out);

// An interval of 'Imitate holding key' on press that the file cannot say (it holds c0 ff ff ff for anything above 16 777 215 ms)
typedef struct {
    uint8_t key;     // the key index
    uint32_t ms;
} cape_compile_hold;

typedef struct {
    int layout;                                    // the physical layout of the keyboard (CAPE_PROFILE_LAYOUT_*), for the lighting
    int convert;                                   // 1: a macro file that is no action of iCUE becomes a Macro (see above)
    void (*random)(void* ctx, uint8_t out[16]);    // 16 random bytes for each new identity (a version 4 UUID is made of them)
    void* random_ctx;
    const cape_compile_hold* hold;                 // intervals of 'Imitate holding key' that win over the base and the file
    size_t nhold;
    const char* const* shapes;                      // optional per-key XML grouping and remap identity, checked against the files
    const cape_lightmodel* lighting;               // optional complete replacement, copied without new identities
    // out
    unsigned converted;                            // macro files that became Macros (converting mode)
    unsigned kept;                                 // actions of the base kept whole
    int effects;                                   // the base's lighting includes animations; all files matched byte for byte
    int relaid;                                    // (converting mode) the files of the bindings are not laid out as iCUE writes
                                                   // them: a save writes them again
} cape_compile_import_opts;

// Imports the files of a slot (PROFILE.MAP, PROFILE.DAT and its macro files, PROFILE.I, the lighting files) into out, with base
// (NULL: none). out is initialised here (cape_profile_free it, also on failure). Returns CAPE_OK, CAPE_COMPILE_E_UNCOMPILABLE or
// CAPE_COMPILE_E_NOCELL (why says what), CAPE_E_ARG (no random source when a new identity is needed), CAPE_E_CAP (memory).
int cape_compile_import(const cape_slot_files* files, const cape_profile_profile* base, cape_compile_import_opts* opts,
                        cape_profile_profile* out, char* why);

#endif  // CAPE_COMPILE_H
