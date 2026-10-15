#ifndef CAPE_PROFILE_H
#define CAPE_PROFILE_H

/*
 * The model of the profile in PROFILE.ZIP (cape_qz.h, cape_xml.h): iCUE's legacy backup of a hardware profile of the Corsair
 * K95 RGB Platinum, the cereal class BackupProfile::Data at version 203, as a tree of typed C structures. This module reads a
 * model from an XML tree and builds an XML tree from a model; turning a model into the other files of the slot is
 * cape_compile.h.
 *
 * A compatibility level is the set of classes, versions, fields and enum values of one version of iCUE that this module knows;
 * there is one, "icue-5.51.40". Reading is strict: whatever the level does not have (a class version, a polymorphic type, an
 * enum value) is CAPE_PROFILE_E_UNKNOWN, a profile written by an unsupported iCUE; whatever does not follow the level's grammar (fields
 * in another order, a malformed number, colour or UUID, the same identity twice) is CAPE_PROFILE_E_CORRUPT. The level does not
 * have the mouse and consumer events of a macro (the hardware editor never writes them), attached actions, or an object shared
 * by two pointers: none of them is in a hardware profile.
 *
 * The cereal mechanics are iCUE's: a class writes its version the first time the document holds it; a polymorphic type is
 * registered with its name the first time (0x80000000 | n), referred to by its number afterwards, and the root properties but
 * the lighting have the static type 0x40000000; the root properties are unique pointers (valid 1), everything else shared
 * pointers numbered in the order of the document. Writing walks the model in the order iCUE does, so the numbers are iCUE's: a
 * model read from iCUE's XML is written back to the same bytes, and two models are equal when they are written to the same bytes
 * (cape_profile_equal).
 *
 * Numbers are written and read in the C notation whatever the locale of the process: integers as decimal, the reals of the
 * lighting with 17 significant digits, the components of a colour with 6, as iCUE's archive does.
 *
 * Memory: a model owns its strings and arrays (cape_profile_free). Functions return CAPE_OK or a negative code: the CAPE_E_* codes
 * of cape_format.h, the CAPE_XML_E_* codes of cape_xml.h (building the tree), or the CAPE_PROFILE_E_* codes below.
 */

#include "cape_format.h"
#include "cape_xml.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_PROFILE_E_UNKNOWN  (-70)   // a class version, a type or a value the level does not have: an unsupported iCUE
#define CAPE_PROFILE_E_CORRUPT  (-71)   // not the grammar of the level
#define CAPE_PROFILE_E_MEMORY   (-72)

#define CAPE_PROFILE_WHY  160   // room for the reason of a failure (a path in the document and what is wrong there)

// Short description of any CAPE_OK, CAPE_E_*, CAPE_XML_E_* or CAPE_PROFILE_E_* code.
const char* cape_profile_strerror(int err);

// The name of the compatibility level (the only one, for now).
#define CAPE_PROFILE_LEVEL  "icue-5.51.40"

// A string: UTF-8, owned by the model, with a terminator after len bytes. A NULL p is the empty string.
typedef struct {
    char* p;
    uint32_t len;
} cape_profile_text;

typedef struct {
    uint8_t b[16];   // in the order they are written, {b0b1b2b3-b4b5-...}
} cape_profile_uuid;

// A colour as iCUE writes it: rgb(r g b) or rgba(r g b a), with components from 0 to 1
typedef struct {
    double c[4];
    uint8_t has_alpha;
} cape_profile_colour;

typedef struct {
    uint8_t present;   // 0: nullopt
    cape_profile_colour colour;
} cape_profile_optcolour;

// Enums: the values are the position of the name in the lists below (the same as iCUE's numbers, where it has them)
enum { CAPE_PROFILE_EVENT_CLICK, CAPE_PROFILE_EVENT_LONG, CAPE_PROFILE_EVENT_EXTRA_LONG, CAPE_PROFILE_EVENT_5S_LONG };
enum { CAPE_PROFILE_LAYER_STANDARD, CAPE_PROFILE_LAYER_FN1, CAPE_PROFILE_LAYER_FN2, CAPE_PROFILE_LAYER_CTRL, CAPE_PROFILE_LAYER_ALT, CAPE_PROFILE_LAYER_SHIFT };
enum {
    CAPE_PROFILE_HINT_ON_BOTH, CAPE_PROFILE_HINT_ON_PRESS, CAPE_PROFILE_HINT_ON_RELEASE, CAPE_PROFILE_HINT_ON_PRESS_LONG,
    CAPE_PROFILE_HINT_ON_PRESS_EXTRA_LONG, CAPE_PROFILE_HINT_ON_PRESS_5S_LONG, CAPE_PROFILE_HINT_WHILE_PRESSED, CAPE_PROFILE_HINT_TOGGLE
};
enum { CAPE_PROFILE_REPEAT_NO, CAPE_PROFILE_REPEAT, CAPE_PROFILE_REPEAT_CONSTANTLY };
enum { CAPE_PROFILE_DELAY_CONSTANT, CAPE_PROFILE_DELAY_RANDOM };
enum { CAPE_PROFILE_HOLD_ON_PRESS, CAPE_PROFILE_HOLD_TOGGLE, CAPE_PROFILE_HOLD_WHILE_PRESSED };
enum { CAPE_PROFILE_SNIPER_ON_OFF, CAPE_PROFILE_SNIPER_WHILE_PRESSED };
enum { CAPE_PROFILE_TERMINATE_UNINTERRUPTED, CAPE_PROFILE_TERMINATE_RESUME, CAPE_PROFILE_TERMINATE_INTERRUPT };
enum {
    CAPE_PROFILE_MEDIA_PLAY_PAUSE, CAPE_PROFILE_MEDIA_STOP, CAPE_PROFILE_MEDIA_PREV, CAPE_PROFILE_MEDIA_NEXT, CAPE_PROFILE_MEDIA_VOLUME_UP,
    CAPE_PROFILE_MEDIA_VOLUME_DOWN, CAPE_PROFILE_MEDIA_MUTE
};
enum { CAPE_PROFILE_SUB_PRESS, CAPE_PROFILE_SUB_RELEASE, CAPE_PROFILE_SUB_STROKE };
enum { CAPE_PROFILE_OPTION_CONCRETE, CAPE_PROFILE_OPTION_ALL };
enum { CAPE_PROFILE_PLAY_STANDARD, CAPE_PROFILE_PLAY_FROM_KEY_CENTER, CAPE_PROFILE_PLAY_ON_KEY_CENTER };
enum { CAPE_PROFILE_SPREAD_BETWEEN_FRONTS, CAPE_PROFILE_SPREAD_AFTER_INNER, CAPE_PROFILE_SPREAD_BEFORE_OUTER, CAPE_PROFILE_SPREAD_EVERY };

// An event of a macro: a delay (constant or random) or a key
typedef enum { CAPE_PROFILE_DELAY_EVENT, CAPE_PROFILE_KEY_EVENT } cape_profile_event_kind;
typedef struct {
    cape_profile_event_kind kind;
    // delay
    uint8_t random;
    uint32_t delay;           // when not random
    uint32_t delay_min;       // when random
    uint32_t delay_max;
    // key
    cape_profile_text* keys;     // iCUE's key names (one, in every event iCUE writes)
    uint32_t nkeys;
    uint8_t sub;              // CAPE_PROFILE_SUB_*
} cape_profile_event;

typedef enum { CAPE_PROFILE_REMAP, CAPE_PROFILE_MACRO, CAPE_PROFILE_TEXT, CAPE_PROFILE_MEDIA } cape_profile_action_kind;

typedef struct {
    cape_profile_action_kind kind;
    // BaseAction
    cape_profile_text name;
    cape_profile_uuid id;
    uint32_t repeat_count;
    uint8_t repeat_mode;         // CAPE_PROFILE_REPEAT_*
    uint32_t repeat_delay;
    uint8_t delay_mode;          // CAPE_PROFILE_DELAY_*
    uint32_t random_delay_from;
    uint32_t random_delay_to;
    uint8_t terminate_on_second;
    uint8_t restart_on_second;
    uint8_t exec_hint;           // CAPE_PROFILE_HINT_*
    uint8_t retain_original;
    cape_profile_uuid lighting;     // actionLighting (null when none)
    cape_profile_text sound_path;   // actionSoundPath
    // KeyRemapAction
    cape_profile_text key_name;     // the destination of a remap ("" with a keystroke)
    cape_profile_text* stroke;      // keyStroke: the keys of a shortcut
    uint32_t nstroke;
    uint8_t holding_enabled;     // Imitate
    uint8_t holding_type;        // CAPE_PROFILE_HOLD_*
    uint32_t holding_interval;   // ms
    cape_profile_text program_path;
    uint8_t sniper_mode;         // CAPE_PROFILE_SNIPER_*
    uint32_t remap_group;
    // MacroAction
    uint8_t termination;         // CAPE_PROFILE_TERMINATE_*
    uint8_t high_priority;
    cape_profile_event* events;
    uint32_t nevents;
    // TextAction
    cape_profile_text text;
    uint32_t char_delay;         // delayBetweenCharacters, ms
    // MediaAction
    uint8_t media;               // CAPE_PROFILE_MEDIA_*
} cape_profile_action;

// An action and the key it is on (KeyAssociation)
typedef struct {
    cape_profile_action action;
    cape_profile_text key;     // "": an action iCUE keeps on no key, which has nothing in the files
    uint8_t layer;      // CAPE_PROFILE_LAYER_*
    uint8_t event;      // CAPE_PROFILE_EVENT_*
    uint32_t distance;
} cape_profile_binding;

typedef struct {
    double time;
    cape_profile_colour colour;
} cape_profile_transition;

typedef enum {
    CAPE_PROFILE_STATIC, CAPE_PROFILE_SOLID, CAPE_PROFILE_GRADIENT, CAPE_PROFILE_WAVE, CAPE_PROFILE_RIPPLE, CAPE_PROFILE_COLOR_PULSE,
    CAPE_PROFILE_COLOR_SHIFT, CAPE_PROFILE_COLOR_WAVE, CAPE_PROFILE_RAIN, CAPE_PROFILE_RAINBOW_WAVE, CAPE_PROFILE_SPIRAL_RAINBOW,
    CAPE_PROFILE_TYPE, CAPE_PROFILE_VISOR
} cape_profile_effect_kind;
#define CAPE_PROFILE_EFFECTS 13

// The effect of a lighting layer; the fields of a kind are those of its class, the others are 0
typedef struct {
    cape_profile_effect_kind kind;
    // CUEAnimationBase
    cape_profile_text name;
    cape_profile_uuid id;
    uint32_t duration;              // ms
    double brightness;
    // CUEAnimationWithTransitions (Static, Solid, Gradient, Wave, Ripple)
    cape_profile_transition* transitions;
    uint32_t ntransitions;
    uint32_t opacity;               // Static, Rain, Type
    double velocity;                // Wave, Ripple
    double tail_length;             // Wave, Ripple
    uint8_t double_sided;           // Wave
    double angle;                   // Wave
    uint8_t auto_duration;          // Ripple
    uint8_t wave_spread;            // Ripple: CAPE_PROFILE_SPREAD_*
    uint32_t colours[2];            // Ripple and the predefined ones with colours: ARGB
    uint8_t random;                 // the predefined ones with colours
    uint32_t speed;                 // the predefined ones
    uint32_t direction;             // ColorWave, Rain, RainbowWave, Visor
    uint8_t clockwise;              // SpiralRainbow
    uint32_t type_duration;         // Type: its own duration (an enum)
    uint32_t type_behaviour;        // Type: typeLightingBehavior
} cape_profile_effect;

// Solid is a list of blocks: each pair has one RGBA colour. Empty is valid; unpaired points are corrupt.
// RGB and opaque RGBA are equivalent here; the original representation is preserved.
int cape_profile_solid_valid(const cape_profile_effect* effect);

typedef struct {
    cape_profile_uuid id;
    cape_profile_text name;
    cape_profile_text* keys;           // iCUE's light names
    uint32_t nkeys;
    uint8_t enabled;
    uint8_t start_on_key_press;
    uint8_t start_with_profile;
    uint8_t stop_on_key_press;
    uint8_t stop_on_key_release;
    int32_t stop_after_times;
    uint8_t play_option;            // CAPE_PROFILE_PLAY_*
    uint8_t option;                 // CAPE_PROFILE_OPTION_*
    cape_profile_effect effect;
} cape_profile_layer;

// Known iCUE editor limits, including muted layers. NULL means valid; otherwise a static error description.
const char* cape_profile_layer_error(const cape_profile_layer* layer);

typedef struct {
    uint8_t advanced;               // isAdvancedMode
    cape_profile_layer* layers;        // in the order of iCUE's list, from the top
    uint32_t nlayers;
    cape_profile_colour brightness;    // BrightnessIndicatorProperty
    cape_profile_optcolour winlock_on; // WinlockIndicatorProperty
    cape_profile_optcolour winlock_off;
    cape_profile_colour m1, m2, m3;    // ProfileIndicatorProperty
    cape_profile_optcolour profile;
    uint8_t disable_alt_tab;        // WinlockProperty
    uint8_t disable_alt_f4;
    uint8_t disable_win;
    uint8_t disable_shift_tab;
    uint8_t disable_encryption;
    uint8_t disable_touchpad;
    uint8_t swap_fkeys;
    uint8_t side_lighting;          // SideLightingProperty
    cape_profile_binding* bindings;    // ActionsProperty
    uint32_t nbindings;
    uint32_t hw_actions_types;      // hwActionsSupportedTypes
    cape_profile_uuid active_preset;   // presetsInfo
    uint8_t preset_active;
} cape_profile_profile;

// Reads the model from a tree (cape_xml_parse) of the level. On failure *p is left empty and why (if not NULL, CAPE_PROFILE_WHY
// bytes) says where and what. Returns CAPE_OK, CAPE_E_ARG, CAPE_PROFILE_E_UNKNOWN, CAPE_PROFILE_E_CORRUPT or CAPE_PROFILE_E_MEMORY.
int cape_profile_read(const cape_xml_tree* t, cape_profile_profile* p, char* why);

// Builds the tree of a model (t is initialised by this function; cape_xml_free it). Returns CAPE_OK, CAPE_E_ARG, a CAPE_XML_E_*
// code (a value the XML cannot hold, a limit), or CAPE_PROFILE_E_CORRUPT (a value the level cannot write: an enum out of its list,
// a colour component out of 0..1, a real that is not finite, an identity null or used twice), with why as above.
int cape_profile_build(const cape_profile_profile* p, cape_xml_tree* t, char* why);

// Whether two models are the same: they build the same document. 1 or 0; a negative code if one cannot be built.
int cape_profile_equal(const cape_profile_profile* a, const cape_profile_profile* b);

// Releases what a model owns and leaves it empty. Safe on an empty model.
void cape_profile_free(cape_profile_profile* p);

#endif  // CAPE_PROFILE_H
