#ifndef CAPE_LIGHT_H
#define CAPE_LIGHT_H

/*
 * The lighting of an on-board profile slot of the Corsair K95 RGB Platinum (USB 1b1c:1b2d), as an image of the
 * keys: what the layer files of a slot (lghtcnt.cnt and lght_NN.d/.k/.r) say, and how it maps onto the LEDs of
 * the daemon's keymap. The format: corsair-protocol formats/cape/lighting.md, lighting-cells.md, custom-effects.md and
 * predefined-effects.md.
 *
 * Like cape_format.h, this module has no dependencies besides the C library and that header: it knows nothing
 * of USB, of the slots as a whole or of the daemon's structures, allocates nothing and keeps no state, so it can
 * be tested without a keyboard. The code that reads the files from the keyboard is in cape_hwload.c.
 *
 * What a slot's lighting is: a list of up to CAPE_LAYERS_MAX layers, painted in file order (lght_00 first), each
 * over the ones before it. A layer is one of
 *  - a static colour: one colour for a set of keys (.k lists the cells, .r holds the colour, its alpha is the
 *    opacity of the layer);
 *  - a custom effect (Solid, Gradient, Ripple, Wave), which the keyboard runs as an animation;
 *  - a predefined effect (Rainbow wave, Rain...), which iCUE only allows alone.
 * Only the static layers make an image. A slot that has an effect is not an image, and the reading code says so
 * instead of showing part of it.
 *
 * Verified on: firmware 3.29, bootloader 3.03. Only what has been seen from iCUE is accepted: anything else is
 * reported as unknown, with the reason, and never guessed.
 */

#include "cape_format.h"

#include <stddef.h>
#include <stdint.h>

// Cells of a layer: one byte each. The keyboard uses 0..162.
#define CAPE_CELLS      256
// Layers a hardware profile can hold (iCUE's limit for the custom kinds; a predefined effect takes the whole profile)
#define CAPE_LAYERS_MAX 5

// LED indices of the daemon's K95 keymap (keymap.c, first table) of the three buttons that have no cell: their
// colours are the indicator colours of PROFILE.I
#define CAPE_LED_PROFILE     0x7d   // profswitch, logo
#define CAPE_LED_BRIGHTNESS  0x89   // light
#define CAPE_LED_WINLOCK     0x08   // lock
// The last LED that has a cell: the top bar is LED 144 to 162
#define CAPE_LED_LAST        162

// The cell of a key in the lighting files, from its LED index in the daemon's K95 keymap (the `led` field, the
// same index that lighting.r/g/b are addressed with):
//   led 144..162 (top bar)            cell = led
//   led % 12 < 8                      cell = 8 * (led / 12) + led % 12
//   led % 12 >= 8 (numpad, media, G)  cell = 8 * (12 + c % 6) + 4 * (c / 6) + (led % 12 - 8), c = led / 12
// Returns -1 for a negative LED and for anything above CAPE_LED_LAST: a cell that does not exist must never
// end up in a .k file that is written to the keyboard. The rule runs from LED to cell, the verified direction;
// the aliases of the keymap (esc/zone1, g1/zone11...) share the LED number and therefore the colour.
int cape_light_cell(int led);

// The cells iCUE lists in the .k of a layer, in the order it lists them (corsair-protocol formats/cape/lighting-cells.md). There
// are CAPE_CANON_COUNT of them, the cells of the keys iCUE lists on this keyboard: the .k of its layer over all keys has exactly
// these, and the .k of every layer iCUE has been seen writing holds some of them in this order. What is not one of them was never
// seen in a .k, which includes the cells of the three buttons (the profile, brightness and Win Lock buttons take their
// colours from PROFILE.I) and of the keys another layout has (the ANSI backslash, the Japanese keys): a writer must never
// produce one. The 116 cells below 144 follow the LED grid of the daemon's keymap read by rows (led % 12 first, then led / 12);
// the 19 cells of the top bar are not in LED order, so the order is kept as a table.
#define CAPE_CANON_COUNT  135
// The position of a cell in that order, 0 to CAPE_CANON_COUNT - 1; -1 for a cell that is not one of them.
int cape_light_canon_rank(int cell);
// The cell at a position of that order; -1 for a position that does not exist.
int cape_light_canon_cell(int rank);
// The LED of the daemon's keymap whose colour a cell shows (the inverse of cape_light_cell() over the listed cells); -1 for
// any other cell.
int cape_light_led(int cell);

// What a layer is, from its descriptor lght_NN.d
typedef enum {
    CAPE_LAYER_UNKNOWN = 0,
    CAPE_LAYER_STATIC,       // 37 bytes, byte for byte the descriptor iCUE writes for a static colour
    CAPE_LAYER_PREDEFINED,   // 13 bytes, effect type 0-6, 8 or 9
    CAPE_LAYER_CUSTOM        // 553 bytes, effect type 10-13
} cape_layer_kind;

// Classifies a descriptor by its size and its type byte. A static descriptor that differs from iCUE's in any
// byte (the flags at 17..20 say when the layer runs, and 00 01 00 00 is the only value seen in 20 layers) is
// unknown, and so is every other size or type. If why is not NULL it gets a short reason for UNKNOWN (empty
// otherwise; cap includes the terminator).
cape_layer_kind cape_light_layer_kind(const uint8_t* d, size_t len, char* why, size_t cap);

// The colour of every cell of a slot's static layers, cells not covered being black
typedef struct {
    uint8_t rgb[CAPE_CELLS][3];
    uint8_t covered[CAPE_CELLS];   // a layer lists the cell (not the same as a colour: a black or transparent layer covers)
} cape_light_image;

void cape_light_image_clear(cape_light_image* img);
// Number of cells some layer lists.
unsigned cape_light_covered(const cape_light_image* img);
// Number of the cells some layer lists that no key of the keyboard has (above CAPE_LED_LAST): they cannot be shown.
// iCUE never writes one.
unsigned cape_light_unmapped(const cape_light_image* img);

// Errors of cape_light_paint, in addition to the CAPE_E_* codes of cape_format.h
#define CAPE_LIGHT_E_MIXED  (-40)  // the colours of the layer differ from key to key: iCUE never writes that
#define CAPE_LIGHT_E_MANY   (-41)  // the layer lists more keys than the 256 colours of its .r
// (the codes of the other direction, CAPE_LIGHT_E_COLOURS and CAPE_LIGHT_E_ORDER, are further down)
// Short description of any CAPE_OK, CAPE_E_* or CAPE_LIGHT_E_* code.
const char* cape_light_strerror(int err);

// Paints one static layer over the image, given its .k and .r as read. Entry i of .r is the colour of the i-th
// cell of .k; a static layer has one colour, so the entries 0..n-1 must be equal (the entries beyond n are
// ignored: the layer over all keys has three too many). The layer is drawn over what is there with its alpha
// as opacity:
//   out = (src * a + dst * (255 - a) + 127) / 255   per component, a = alpha
// which is what the observations match (a 100 % layer covers, a 50 % one blends). The keyboard's own arithmetic
// is not known, so this is for showing a slot, not for anything that is written back. A cell listed twice in
// .k is painted once (a layer covers a set of cells) and counted in *dups (which may be NULL). On failure
// the image is left as it was. Returns CAPE_OK or a CAPE_E_* / CAPE_LIGHT_E_* code.
int cape_light_paint(cape_light_image* img, const uint8_t* k, size_t klen, const uint8_t* r, size_t rlen, unsigned* dups);

// The four indicator colours of PROFILE.I (bytes 253, 256, 259 and 262), in this order
enum { CAPE_IND_PROFILE, CAPE_IND_BRIGHTNESS, CAPE_IND_LOCK_ON, CAPE_IND_LOCK_OFF, CAPE_IND_COUNT };
void cape_light_indicators(const cape_info* info, uint8_t out[CAPE_IND_COUNT][3]);

// The colour of each LED of the daemon's keymap, in r[], g[] and b[] addressed by LED index, for the LEDs
// 0..n-1 that have a cell (the others are left alone). Then, if ind is not NULL, the three buttons that are not
// cells take the indicator colours: profswitch the profile colour, light the brightness colour, lock the Win
// Lock OFF colour (the button at rest; ON is for when Win Lock is active). They win over the cell.
void cape_light_leds(const cape_light_image* img, const uint8_t ind[CAPE_IND_COUNT][3], size_t n, uint8_t* r, uint8_t* g, uint8_t* b);

// What the lighting files of a slot amount to
typedef enum {
    CAPE_LIGHT_UNREAD = 0,
    CAPE_LIGHT_STATIC,    // 1 to CAPE_LAYERS_MAX static layers and nothing else: the image is valid
    CAPE_LIGHT_EMPTY,     // no layers: every key is dark
    CAPE_LIGHT_EFFECTS,   // a predefined effect, or custom effect layers (with or without static ones): no image
    CAPE_LIGHT_UNKNOWN    // anything not seen from iCUE, with the reason in why
} cape_light_state;

#define CAPE_LIGHT_WHY  128
#define CAPE_LIGHT_FILE_NAME  12   // "lghtcnt.cnt", "lght_NN.d" and a terminator

typedef struct {
    cape_light_state state;
    unsigned layers;                                   // lghtcnt.cnt
    unsigned n_static, n_predefined, n_custom;         // set when the kinds of the layers are known
    uint8_t predefined[CAPE_LIGHT_DESC_PREDEFINED];    // the descriptor of the predefined effect, if n_predefined
    cape_light_image image;                            // valid if state is CAPE_LIGHT_STATIC
    uint8_t ind[CAPE_IND_COUNT][3];                    // indicator colours of PROFILE.I
    int ind_ok;                                        // ind was filled
    unsigned dup_cells;                                // cells listed twice in one layer, in all
    char why[CAPE_LIGHT_WHY];                          // for UNKNOWN the reason, for EFFECTS what the layers are
    unsigned packets;                                  // packets the reading of the lighting took (set by the reader)
    cape_layer_kind kind[CAPE_LAYERS_MAX];             // the kind of each layer, when the descriptors were read
    int complete;                                      // every file the layers make up was read, so that they can be written back as
                                                       // they are: always for STATIC and EMPTY, for EFFECTS only by cape_light_read_all
    char missing[CAPE_LIGHT_FILE_NAME];                // for EFFECTS that are not complete: the file that was missing or too large
} cape_light_slot;

void cape_light_slot_init(cape_light_slot* s);
const char* cape_light_state_name(cape_light_state st);
// The slot cannot be understood: state UNKNOWN with the reason (copied, truncated to CAPE_LIGHT_WHY).
void cape_light_slot_unknown(cape_light_slot* s, const char* why);
// lghtcnt.cnt says n layers. Returns 1 if the descriptors of the layers are to be read, 0 if the state is
// final: EMPTY for none, UNKNOWN for more than CAPE_LAYERS_MAX.
int cape_light_slot_count(cape_light_slot* s, uint32_t n);
// The kinds of the n layers, in file order, none of them CAPE_LAYER_UNKNOWN (that ends the reading earlier, with
// cape_light_slot_unknown). A predefined effect is only valid alone; custom effect layers, alone or with static
// ones, make EFFECTS. Returns 1 if all the layers are static, so that the caller reads their .k and .r, paints
// them in order and then sets the state to CAPE_LIGHT_STATIC; 0 if the state is final.
int cape_light_slot_kinds(cape_light_slot* s, const cape_layer_kind* kinds, unsigned n);

// Where the files of a slot come from: the reader below asks for them by name ("lghtcnt.cnt", "lght_01.k"...).
// Returns 0 with the file in buf and its length in *len; 1 if the file does not exist or is larger than cap
// (either way it is not one of ours); a negative value if the reading itself failed, which ends the reading.
typedef int (*cape_light_source)(void* ctx, const char* name, uint8_t* buf, size_t cap, size_t* len);

// Reads the lighting of one slot from src into s, which need not be cleared: what it held is forgotten, but for the
// indicator colours (ind and ind_ok, which are the caller's business). Two passes, so that a slot that is not an
// image costs as little as possible:
// lghtcnt.cnt, then the descriptor of every layer, and only if all of them are static the .k and .r of each,
// painted in file order. A file that is missing, too large or not understood ends in CAPE_LIGHT_UNKNOWN with the
// reason in s->why and is not an error. Returns CAPE_OK, or the negative value src returned (s is then not to be used).
int cape_light_read(cape_light_slot* s, cape_light_source src, void* ctx);
// The same, and for a slot of effects also every file its layers are made of, so that they can be kept byte for byte by a save that
// does not change the lighting (hwslot): a predefined effect is its .d, a custom effect its .d and .k, a static layer its .d, .k
// and .r, also among effects (the order iCUE writes them in, and the files of all its saves that have effects). complete says
// whether they all were there; if one was not, missing names it and the state stays EFFECTS.
int cape_light_read_all(cape_light_slot* s, cape_light_source src, void* ctx);

// ---------------------------------------------------------------------------------------------------------
// The other direction: the files iCUE would write for a picture of static colours

// What a picture must be to be written. A slot holds CAPE_LAYERS_MAX layers and a static layer has one colour, so a picture can
// have that many colours other than black (black is a key that no layer lists: the keyboard looks the same, and the layer
// would use up one of the five), on the CAPE_CANON_COUNT cells of the keys iCUE lists. Anything else is not stored: the LEDs of
// the daemon's keymap that have no such cell (the keys of another layout, the keys a K95P does not have, the three buttons)
// are left out and not counted. Every layer is written with an alpha of ff, one layer per colour, its cells in canonical order.
#define CAPE_LIGHT_E_COLOURS  (-42)  // more colours other than black than layers: the picture cannot be stored
#define CAPE_LIGHT_E_ORDER    (-43)  // (cape_light_pack_ordered) a colour the picture draws is not in the order that was given

typedef struct {
    uint8_t rgb[3];
    uint8_t cells[CAPE_CANON_COUNT];   // canonical order
    unsigned n;
} cape_light_layer;

typedef struct {
    cape_light_layer layer[CAPE_LAYERS_MAX];
    unsigned layers;      // the layers to write, in the order they are written; 0 when the picture cannot be stored
    unsigned colours;     // the colours other than black the cells have, even if there are too many (then layers is 0)
    unsigned lit;         // the cells that have one
    unsigned ignored;     // LEDs up to CAPE_LED_LAST that are not black and have no cell in a .k (buttons excepted): not written
} cape_light_pack_result;

// Turns the colours of the LEDs (r[led], g[led], b[led], for the n LEDs 0..n-1; the ones beyond n are black) into layers. The
// layers come in the order of the first cell of each colour in the canonical order, which gives the same files for the same
// picture whatever the order in which it was drawn. CAPE_OK, CAPE_E_ARG, or CAPE_LIGHT_E_COLOURS (out->colours says how many).
int cape_light_pack(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, cape_light_pack_result* out);
// The same with the layers in the order of `order` (norder colours, RGB): the order iCUE's own save had, for the tests that
// compare files byte for byte. A colour of the order that the picture does not draw is skipped; one it draws and the order lacks
// is CAPE_LIGHT_E_ORDER.
int cape_light_pack_ordered(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n, const uint8_t (*order)[3], size_t norder,
                            cape_light_pack_result* out);

// The three buttons that have no cell take their colours from PROFILE.I (cape_light_leds()), and a picture that paints them
// differently cannot be written as it is. Which of them differ from the indicator colours ind, as a mask of CAPE_BTN_*
// (0 if ind is NULL). A LED beyond n is black.
#define CAPE_BTN_PROFILE     0x01
#define CAPE_BTN_BRIGHTNESS  0x02
#define CAPE_BTN_WINLOCK     0x04   // the Win Lock button at rest: the OFF colour
unsigned cape_light_buttons_changed(const uint8_t* r, const uint8_t* g, const uint8_t* b, size_t n,
                                    const uint8_t ind[CAPE_IND_COUNT][3]);

// A file of the lighting of a slot: "lghtcnt.cnt" and, for each layer, "lght_NN.d", ".k" and ".r"
#define CAPE_LIGHT_FILES_MAX  (1 + 3 * CAPE_LAYERS_MAX)
typedef struct {
    char name[CAPE_LIGHT_FILE_NAME];
    uint8_t data[CAPE_COLORS_SIZE];
    size_t len;
} cape_light_file;

// The files for a packed picture, in the order iCUE writes them: each layer's .d, .k and .r from the layer at the bottom, then
// lghtcnt.cnt (alone, with a count of 0, for a picture without layers). files holds cap entries, CAPE_LIGHT_FILES_MAX are
// always enough; *n gets the number written. CAPE_OK, CAPE_E_ARG, CAPE_E_RANGE (more layers than a profile holds) or CAPE_E_CAP.
int cape_light_files_build(const cape_light_pack_result* pack, cape_light_file* files, size_t cap, size_t* n);

#endif  // CAPE_LIGHT_H
