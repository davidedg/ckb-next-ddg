#ifndef CAPE_NAMES_H
#define CAPE_NAMES_H

/*
 * The names iCUE gives the keys, the lights and the Media actions of the Corsair K95 RGB Platinum in PROFILE.ZIP (cape_xml.h),
 * and what they are in the other files of a slot: the key of PROFILE.MAP, PROFILE.DAT and the macro files (an index of the
 * first 152 entries of the daemon's keymap, or one of the mouse buttons CAPE_KEY_MOUSE1..5 of cape_format.h), and the LED of
 * the daemon's keymap whose cell a lighting layer lists (cape_light.h turns a LED into a cell).
 *
 * The names are those of iCUE's description of this keyboard, for every layout it has (US, UK, BR, JP, KR), and the ones its
 * hardware profiles use besides: the mouse buttons and the Language keys of a remap. Keys and lights are two different sets of
 * names: as a light, "G3" is the light of the G1 key (iCUE names the six G lights G3, G6, G9, G12, G15, G18), as a key it is
 * the G3 key; the top bar has lights (Led_TopZone1..19) and no keys.
 *
 * Every lookup is by name and length (the names of a parsed tree are not terminated). No state, no allocation, only the C
 * library.
 */

#include <stddef.h>

#define CAPE_PROFILE_NONE  (-1)

// The key of the files for iCUE's name of a key, or CAPE_PROFILE_NONE for a name that is not one of this keyboard. The Language
// keys a remap offers that the K95P does not have (Lang3..9, International6..9, KeypadComma) are written by iCUE as Esc, and so
// they are here.
int cape_profile_key(const char* name, size_t len);

// iCUE's name of a key of the files (Escape for 0, Mouse1..5 for the mouse buttons), or NULL for a key iCUE has no name for.
const char* cape_profile_key_name(int key);

// The LED of iCUE's name of a light of a lighting layer, or CAPE_PROFILE_NONE for a name that is not one of this keyboard, or a light
// with no LED in the keymap (Lang1, Lang2). Whether the LED has a cell that a layer can list is for cape_light to say.
int cape_profile_led(const char* name, size_t len);

// iCUE's name of the light of a LED, or NULL.
const char* cape_profile_led_name(int led);

// The media key of the files that a Media action of iCUE is written as, or CAPE_PROFILE_NONE.
int cape_profile_media(const char* name, size_t len);

// iCUE's Media action for a media key of the files, or NULL.
const char* cape_profile_media_name(int key);

// The physical layouts of the keyboard, numbered as the daemon numbers them (the layout byte of the keyboard's identification
// plus one, LAYOUT_ANSI .. LAYOUT_DUBEOLSIK of structures.h) and as iCUE does; the names of iCUE's description in brackets.
#define CAPE_PROFILE_LAYOUT_ANSI  1   // (US)
#define CAPE_PROFILE_LAYOUT_ISO   2   // (UK)
#define CAPE_PROFILE_LAYOUT_ABNT  3   // (BR)
#define CAPE_PROFILE_LAYOUT_JIS   4   // (JP)
#define CAPE_PROFILE_LAYOUT_KS    5   // (KR)

// The most lights a lighting layer can hold, on any layout (JIS)
#define CAPE_PROFILE_LAYER_MAX    138

// A light a lighting layer can hold on a layout: returns its cell in the lighting files, and sets order (if not NULL) to its place
// in the order iCUE lists the lights of a layer in, the same on every layout (corsair-protocol formats/cape/lighting-cells.md).
// CAPE_PROFILE_NONE for a name that is not such a light of that layout: one of another layout (the ANSI backslash on an ISO
// keyboard), one of the three buttons whose colours are in PROFILE.I, any other name, or a layout that is not one of the five.
// On a layout the lights have different cells, but one cell can be two lights of two layouts (JIS International5 and KS Lang2),
// at two places of the order.
int cape_profile_light(const char* name, size_t len, int layout, int* order);

// The name of the light of a cell on a layout, or NULL.
const char* cape_profile_light_name(int cell, int layout);

// The cells of a layer over all keys of a layout, in iCUE's order: returns their number (0 for a layout that is not one of the
// five) and sets colours (if not NULL) to the number of colours iCUE writes for that layer, which counts the three buttons too.
unsigned cape_profile_layout_all(int layout, unsigned char cells[CAPE_PROFILE_LAYER_MAX], unsigned* colours);

// LED anchors used for animation origins. Includes the three protected indicators for AllLeds.
// Returns 1 on success, 0 for an unknown light/layout; outputs are unchanged on failure.
int cape_profile_anchor(const char* name, size_t len, int layout, int* x, int* y);
// All anchors of a layout in device order; returns the count, or zero for an unknown layout.
unsigned cape_profile_anchors(int layout, int x[CAPE_PROFILE_LAYER_MAX + 3], int y[CAPE_PROFILE_LAYER_MAX + 3]);

#endif  // CAPE_NAMES_H
