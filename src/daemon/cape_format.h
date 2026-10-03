#ifndef CAPE_FORMAT_H
#define CAPE_FORMAT_H

/*
 * File formats of the on-board profile slots of the Corsair K95 RGB Platinum (USB 1b1c:1b2d).
 *
 * "CAPE" is the name iCUE's error strings use for the small file system in the keyboard's flash. Each of
 * the three profile slots holds a handful of files: key remaps, macros, lighting and profile information.
 * This module only converts between the bytes of those files and C structures. It knows nothing about USB,
 * slots, ckb-next profiles or key names, has no dependencies besides the C library, allocates nothing and
 * keeps no state, so it can be tested without a keyboard.
 *
 * Verified on: firmware 3.29, bootloader 3.03 (see cape_fw.h). Nothing here is known to hold for other
 * firmware levels.
 *
 * Conventions
 *  - Integers in the files are little-endian, except where a format says otherwise (the event count of a
 *    macro file is big-endian). Key numbers in PROFILE.MAP, PROFILE.DAT and the macro files are indices into
 *    the daemon's keymap[] (the same numbering as the key input packets); the mouse buttons have the special
 *    codes CAPE_KEY_MOUSE1..5. The lighting files number things differently (see below).
 *  - Every codec is lossless: bytes we do not interpret (reserved fields, padding, unknown event types, the
 *    tail of the lighting descriptor...) are kept in the structures, so for any input that a parse function
 *    accepts, the matching build function reproduces exactly the same bytes.
 *  - Parse functions are strict about the structure (container type and version, exact length) and
 *    permissive about the values: any subtype, trigger, run type or event byte is accepted and left to the
 *    caller to interpret.
 *  - Functions return a count, a number of bytes, or CAPE_OK on success, and a negative CAPE_E_* value on
 *    failure. Nothing is ever written beyond the capacity given by the caller. Checks are made in this
 *    order: NULL arguments (CAPE_E_ARG), minimum length (CAPE_E_SIZE), container type (CAPE_E_MAGIC),
 *    length against the header (CAPE_E_SIZE), value limits (CAPE_E_RANGE), output capacity (CAPE_E_CAP).
 *  - The four functions that return nothing (cape_dat_name, cape_info_init, cape_light_desc_predefined,
 *    cape_light_desc_static) cannot report errors: their pointer arguments must not be NULL.
 */

#include <stddef.h>
#include <stdint.h>

#define CAPE_OK        0
#define CAPE_E_ARG    (-1)  // NULL pointer, or NULL with a non-zero count
#define CAPE_E_SIZE   (-2)  // length does not match what the header (or the format) says
#define CAPE_E_MAGIC  (-3)  // wrong container type or version byte
#define CAPE_E_RANGE  (-4)  // a value cannot be represented in the file (count, size, delay, name...)
#define CAPE_E_CAP    (-5)  // the caller's output buffer or array is too small

// Short description of an error code, for log messages.
const char* cape_strerror(int err);

// Number of profile slots of the keyboard.
#define CAPE_SLOTS 3

// File names inside a slot.
#define CAPE_FILE_MAP         "PROFILE.MAP"
#define CAPE_FILE_DAT         "PROFILE.DAT"
#define CAPE_FILE_INFO        "PROFILE.I"
#define CAPE_FILE_ZIP         "PROFILE.ZIP"  // iCUE's own copy of the profile; the firmware does not read it
#define CAPE_FILE_LAYERCOUNT  "lghtcnt.cnt"

// Mouse buttons as destination of a remap. They are outside the range of the keyboard keys.
#define CAPE_KEY_MOUSE1  0xc8  // left
#define CAPE_KEY_MOUSE2  0xc9  // right
#define CAPE_KEY_MOUSE3  0xca  // middle
#define CAPE_KEY_MOUSE4  0xcb  // back
#define CAPE_KEY_MOUSE5  0xcc  // forward

// Values seen in the files. The names follow the enumerations of iCUE's hardware profile classes.
enum {
    CAPE_MACRO_MACRO = 0,          // recorded macro
    CAPE_MACRO_SHORTCUT = 1,       // key combination
    CAPE_MACRO_MEDIA = 2,          // not written by iCUE for this keyboard
    CAPE_MACRO_TEXT = 3,           // typed text
    CAPE_MACRO_MOUSEKEYREMAP = 4   // not written by iCUE for this keyboard
};
enum {
    CAPE_START_ONPRESS = 0x00,
    CAPE_START_ONRELEASE = 0x11
};
enum {
    CAPE_RUN_UNINTERRUPTED = 1,    // runs once
    CAPE_RUN_QUEUED = 2,           // not offered by iCUE
    CAPE_RUN_WHILEPRESSED = 3,     // runs once and is aborted when the key is released
    CAPE_RUN_ONTOGGLE = 4,
    CAPE_RUN_SMARTSTOP = 5         // not offered by iCUE
};
enum {
    CAPE_FX_COLORSHIFT = 0,
    CAPE_FX_COLORPULSE = 1,
    CAPE_FX_SPIRALRAINBOW = 2,
    CAPE_FX_RAINBOWWAVE = 3,
    CAPE_FX_COLORWAVE = 4,
    CAPE_FX_VISOR = 5,
    CAPE_FX_RAIN = 6,
    CAPE_FX_STATIC = 7,            // per-key colours, described by the .k and .r files
    CAPE_FX_TYPELIGHTINGGRADIENT = 8,
    CAPE_FX_TYPELIGHTINGRIPPLE = 9,
    CAPE_FX_ADVANCEDSOLID = 10,
    CAPE_FX_ADVANCEDGRADIENT = 11,
    CAPE_FX_ADVANCEDRIPPLE = 12,
    CAPE_FX_ADVANCEDWAVE = 13,
    CAPE_FX_RECORDEDLIGHTING = 14
};
enum { CAPE_SPEED_UNDEFINED = 0, CAPE_SPEED_SLOW = 1, CAPE_SPEED_MEDIUM = 2, CAPE_SPEED_FAST = 3 };
enum {
    CAPE_COLORTYPE_UNDEFINED = 0,
    CAPE_COLORTYPE_RANDOM = 1,
    CAPE_COLORTYPE_SELECTED = 2,
    CAPE_COLORTYPE_ALTERNATING = 3
};
enum {
    CAPE_DIR_UNDEFINED = 0,
    CAPE_DIR_LEFT = 1,
    CAPE_DIR_RIGHT = 2,
    CAPE_DIR_UP = 3,
    CAPE_DIR_DOWN = 4,
    CAPE_DIR_CLOCKWISE = 5,
    CAPE_DIR_COUNTERCLOCKWISE = 6,
    CAPE_DIR_FROMCENTER = 7
};

/*
 * PROFILE.MAP: key remaps.
 *
 *   41 00 | count u16 | count * { source key, destination key, flags }
 *
 * Every remap written by iCUE has flags 0x80. Entries with bit 7 clear are ignored by the firmware. A key
 * that has an entry in PROFILE.DAT does not appear here. Duplicated sources are not checked.
 */
#define CAPE_MAP_HEADER       4
#define CAPE_MAP_ENTRY_SIZE   3
#define CAPE_MAP_FLAG_REMAP   0x80

typedef struct {
    uint8_t src;
    uint8_t dst;
    uint8_t flags;
} cape_map_entry;

// Returns the number of entries. With out == NULL only the count is returned.
int cape_map_parse(const uint8_t* buf, size_t len, cape_map_entry* out, size_t cap);
// Returns the number of bytes written.
int cape_map_build(const cape_map_entry* in, size_t n, uint8_t* out, size_t cap);

/*
 * PROFILE.DAT: the Win Lock options and the table of the keys that run a macro.
 *
 *   50 | Win Lock options | count u16 | count * 16 bytes:
 *     0      source key
 *     1-4    name of the macro file ("M000")
 *     5-7    size of the macro file, u24
 *     8      start condition: 0x00 on press, 0x11 on release
 *     9      run type: 1 uninterrupted, 3 while pressed (2, 4 and 5 exist in iCUE's enumeration)
 *     10     repeat count, always 1
 *     11-15  zeros
 *
 * Byte 1 is a bit mask of what the keyboard disables while Win Lock is on, the Performance options of iCUE
 * (corsair-protocol formats/cape/profile-dat.md). iCUE's default is CAPE_WINLOCK_DEFAULT; any value is accepted and kept, so that a file that
 * holds one nobody has seen is not taken for something else and can be written back as it was.
 */
#define CAPE_WINLOCK_WINKEY     0x01  // Windows key
#define CAPE_WINLOCK_ALT_TAB    0x02
#define CAPE_WINLOCK_ALT_F4     0x04
#define CAPE_WINLOCK_SHIFT_TAB  0x08
#define CAPE_WINLOCK_KNOWN      0x0f  // the bits iCUE has options for
#define CAPE_WINLOCK_DEFAULT    CAPE_WINLOCK_WINKEY
#define CAPE_DAT_HEADER       4
#define CAPE_DAT_ENTRY_SIZE   16
#define CAPE_DAT_SIZE_MAX     0xffffffu

typedef struct {
    uint8_t key;
    uint8_t name[4];     // as in the file, not NUL-terminated: use cape_dat_name()
    uint32_t size;       // at most CAPE_DAT_SIZE_MAX
    uint8_t start;
    uint8_t runtype;
    uint8_t repeat;
    uint8_t reserved[5];
} cape_dat_entry;

// Returns the number of entries. With out == NULL only the count is returned. The Win Lock options are not
// checked (any byte 1 is accepted): cape_dat_winlock() reads them.
int cape_dat_parse(const uint8_t* buf, size_t len, cape_dat_entry* out, size_t cap);
// The Win Lock options (byte 1) of a file that cape_dat_parse() accepts, with the same errors. Returns CAPE_OK.
int cape_dat_winlock(const uint8_t* buf, size_t len, uint8_t* winlock);
// Returns the number of bytes written. cape_dat_build() writes the default Win Lock options.
int cape_dat_build_ex(uint8_t winlock, const cape_dat_entry* in, size_t n, uint8_t* out, size_t cap);
int cape_dat_build(const cape_dat_entry* in, size_t n, uint8_t* out, size_t cap);
// Copies the macro file name into a NUL-terminated string.
void cape_dat_name(const cape_dat_entry* entry, char out[5]);

/*
 * Macro files (M000, M001... M009, M00a...: iCUE numbers them in hex): what a key does when pressed.
 *
 *   'M' | subtype | event count, u24 big-endian | 3 reserved bytes (zero) | the events
 *
 * The count is big-endian: iCUE's Text of 751 events starts 4d 03 00 02 ef 00 00 00 (bytes 3 and 4 are proven,
 * byte 2 is taken as the high byte of the same field and has always been zero, as bytes 5 to 7).
 *
 * Events, executed in order (a key can be pressed before the previous one is released). An event is 2 bytes, or 4 when
 * bit 6 of its first byte is set, as iCUE reads them (corsair-protocol formats/cape/macro.md), so the
 * count of the header is a count of events, not of byte pairs.
 *   20 kk          press key kk
 *   00 kk          release key kk
 *   8h ll          wait ((h & 0x1f) << 8 | ll) milliseconds: 13 bits, iCUE writes it below 8192 ms
 *   c0 hh mm ll    wait the 24 bits hh mm ll milliseconds: iCUE writes it from 8192 ms
 *   e0 aa ab bb    wait a random time from aa << 4 | a to b << 8 | bb milliseconds, two 12-bit bounds (0 to 500 ms
 *                  is e0 00 01 f4); iCUE offers it in a Macro, the firmware runs it
 * A first byte a0..bf is a 2-byte delay iCUE reads 13 bits of but never writes: CAPE_EV_ODD_SHORT, kept as it is, never made. Anything else is CAPE_EV_UNKNOWN, 2 or 4 bytes by the
 * same rule. There is no end marker and no key repeat. iCUE's Text and Keystroke actions are macros too, with the
 * subtypes CAPE_MACRO_TEXT and CAPE_MACRO_SHORTCUT. The delay between the characters of a Text action is stored as delay
 * events.
 */
#define CAPE_MACRO_HEADER  8
#define CAPE_EVENT_SIZE    2           // the shortest event
#define CAPE_EVENT_SIZE_MAX 4
#define CAPE_DELAY_SHORT_MAX 0x1fff    // the longest delay of 2 bytes
#define CAPE_DELAY_MAX     0xffffff    // the longest delay of all
#define CAPE_RANDOM_MAX    0xfff       // each bound of a random delay

typedef struct {
    uint8_t subtype;
    uint8_t reserved[3];   // bytes 5 to 7
} cape_macro_hdr;

// One event. The bytes past its length (cape_ev_size) are always zero: the parser and the functions below make them so, and
// whoever fills one by hand must zero it first.
typedef struct {
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;
} cape_event;

enum { CAPE_EV_UNKNOWN = 0, CAPE_EV_PRESS, CAPE_EV_RELEASE, CAPE_EV_DELAY, CAPE_EV_LONG, CAPE_EV_RANDOM, CAPE_EV_ODD_SHORT };

// The length of an event from its first byte: 4 when bit 6 is set, else 2 (iCUE's reader)
static inline unsigned cape_ev_size_of(uint8_t b0){
    return (b0 & 0x40) ? 4u : 2u;
}
static inline unsigned cape_ev_size(cape_event e){
    return cape_ev_size_of(e.b0);
}

static inline int cape_ev_kind(cape_event e){
    if(e.b0 == 0x20)
        return CAPE_EV_PRESS;
    if(e.b0 == 0x00)
        return CAPE_EV_RELEASE;
    if(e.b0 >= 0x80 && e.b0 <= 0x9f)
        return CAPE_EV_DELAY;
    if(e.b0 >= 0xa0 && e.b0 <= 0xbf)
        return CAPE_EV_ODD_SHORT;
    if(e.b0 == 0xc0)
        return CAPE_EV_LONG;
    if(e.b0 == 0xe0)
        return CAPE_EV_RANDOM;
    return CAPE_EV_UNKNOWN;
}
// Whether two events are the same: their own bytes only
static inline int cape_ev_equal(cape_event a, cape_event b){
    return a.b0 == b.b0 && a.b1 == b.b1 && (cape_ev_size(a) == 2 || (a.b2 == b.b2 && a.b3 == b.b3));
}
// Key of a press or release event.
static inline uint8_t cape_ev_key(cape_event e){
    return e.b1;
}
// Length of a delay in milliseconds: DELAY and ODD_SHORT (13 bits, as iCUE's reader), LONG (24 bits); 0 for anything else
static inline unsigned cape_ev_delay_ms(cape_event e){
    switch(cape_ev_kind(e)){
    case CAPE_EV_DELAY:
    case CAPE_EV_ODD_SHORT:
        return ((unsigned)(e.b0 & 0x1f) << 8) | e.b1;
    case CAPE_EV_LONG:
        return ((unsigned)e.b1 << 16) | ((unsigned)e.b2 << 8) | e.b3;
    default:
        return 0;
    }
}
// The bounds of a random delay in milliseconds
static inline unsigned cape_ev_random_min(cape_event e){
    return ((unsigned)e.b1 << 4) | (e.b2 >> 4);
}
static inline unsigned cape_ev_random_max(cape_event e){
    return ((unsigned)(e.b2 & 0x0f) << 8) | e.b3;
}
static inline cape_event cape_ev_press(uint8_t keycode){
    cape_event e = { 0x20, keycode, 0, 0 };
    return e;
}
static inline cape_event cape_ev_release(uint8_t keycode){
    cape_event e = { 0x00, keycode, 0, 0 };
    return e;
}
// Makes a delay event as iCUE writes it: 2 bytes below 8192 ms, 4 from there. Returns CAPE_E_RANGE if ms > CAPE_DELAY_MAX.
static inline int cape_ev_delay(unsigned ms, cape_event* out){
    if(!out)
        return CAPE_E_ARG;
    if(ms > CAPE_DELAY_MAX)
        return CAPE_E_RANGE;
    if(ms <= CAPE_DELAY_SHORT_MAX){
        cape_event e = { (uint8_t)(0x80 | (ms >> 8)), (uint8_t)(ms & 0xff), 0, 0 };
        *out = e;
    } else {
        cape_event e = { 0xc0, (uint8_t)(ms >> 16), (uint8_t)(ms >> 8), (uint8_t)ms };
        *out = e;
    }
    return CAPE_OK;
}
// Makes a random delay event. Returns CAPE_E_RANGE if a bound is above CAPE_RANDOM_MAX or min > max.
static inline int cape_ev_random(unsigned min, unsigned max, cape_event* out){
    if(!out)
        return CAPE_E_ARG;
    if(min > CAPE_RANDOM_MAX || max > CAPE_RANDOM_MAX || min > max)
        return CAPE_E_RANGE;
    cape_event e = { 0xe0, (uint8_t)(min >> 4), (uint8_t)(((min & 0x0f) << 4) | (max >> 8)), (uint8_t)max };
    *out = e;
    return CAPE_OK;
}

// Returns the number of events. With out == NULL only the count is returned. The bytes of the events must add up to the length
// of the file (CAPE_E_SIZE otherwise).
int cape_macro_parse(const uint8_t* buf, size_t len, cape_macro_hdr* hdr, cape_event* out, size_t cap);
// The bytes the events take in a file
size_t cape_macro_bytes(const cape_event* ev, size_t n);
// Returns the number of bytes written. CAPE_E_RANGE for more than 0xffffff events, CAPE_E_CAP if they do not fit (out is then
// untouched).
int cape_macro_build(const cape_macro_hdr* hdr, const cape_event* ev, size_t n, uint8_t* out, size_t cap);

// Names of the macro files, numbered in lower-case hex as iCUE does: "M000" to "Mfff" (out must hold 5 bytes).
int cape_macro_filename(unsigned idx, char out[5]);
// The other way: the number of a macro file name, 0 to 0xfff, for exactly the names cape_macro_filename() makes (M and three
// lower-case hex digits, then the terminator). CAPE_E_ARG for NULL, CAPE_E_RANGE for any other string. M000 is 0: test < 0.
int cape_macro_filename_parse(const char* name);

/*
 * PROFILE.I: profile information, 268 bytes.
 *
 *   0-1     49 00
 *   2-17    GUID, the same 16 bytes as in the slot table and in the 07 15 command
 *   18-19   cookie u16, the revision counter iCUE increments on every save
 *   20-21   zeros
 *   22-252  name, UTF-16LE, zero-terminated, zero-padded (the longest name iCUE accepts is not known)
 *   253-267 the four indicator colours, RGB (profile, brightness, Win Lock on, Win Lock off; iCUE's defaults are
 *           ff 00 00, ff ff ff, 00 ff ff, ff 00 00) and 3 zero bytes
 */
#define CAPE_INFO_SIZE         268
#define CAPE_INFO_NAME_AREA    231
#define CAPE_INFO_TAIL_SIZE    15
#define CAPE_INFO_NAME_UNITS   115  // UTF-16 units that fit in the name area
#define CAPE_INFO_NAME_MAX     114  // longest name cape_info_set_name() accepts, always leaves a terminator

typedef struct {
    uint8_t guid[16];
    uint16_t cookie;
    uint8_t pad[2];
    uint8_t name_area[CAPE_INFO_NAME_AREA];
    uint8_t tail[CAPE_INFO_TAIL_SIZE];
} cape_info;

// Returns CAPE_OK.
int cape_info_parse(const uint8_t* buf, size_t len, cape_info* out);
// Returns CAPE_INFO_SIZE.
int cape_info_build(const cape_info* in, uint8_t* out, size_t cap);
// Zero GUID, cookie and name, and the constant tail iCUE writes.
void cape_info_init(cape_info* info);
// Copies the name into out, NUL-terminated (cap_units must be at least the length + 1). Returns the length.
int cape_info_get_name(const cape_info* info, uint16_t* out, size_t cap_units);
// Replaces the name (at most CAPE_INFO_NAME_MAX units, no embedded zero unit). Touches nothing else.
int cape_info_set_name(cape_info* info, const uint16_t* name, size_t units);

/*
 * Slot identity: 20 bytes, the entries of the slot table returned by "0e 17 04" (three of them, from byte 4
 * of the reply) and the payload of the "07 15 <slot> 00" command that sets them.
 *
 *   0-15    GUID (an empty slot has all zeros)
 *   16-17   cookie u16
 *   18-19   00 01
 */
#define CAPE_SLOTID_SIZE  20

typedef struct {
    uint8_t guid[16];
    uint16_t cookie;
    uint8_t flags[2];
} cape_slotid;

// Returns CAPE_OK.
int cape_slotid_parse(const uint8_t* buf, size_t len, cape_slotid* out);
// Returns CAPE_SLOTID_SIZE.
int cape_slotid_build(const cape_slotid* in, uint8_t* out, size_t cap);
// 1 if the GUID is all zeros (the slot holds no profile), 0 otherwise.
int cape_slotid_is_empty(const cape_slotid* id);
// The 60 bytes that follow the 4-byte header of the "0e 17 04" reply. Returns CAPE_OK.
int cape_slot_table_parse(const uint8_t* buf, size_t len, cape_slotid out[CAPE_SLOTS]);

/*
 * Lighting files of a slot. lghtcnt.cnt holds the number of layers (u32; iCUE writes at most 5) and each layer has
 * a descriptor lght_NN.d, numbered from the layer at the bottom of iCUE's list; the files are written in that order
 * and the keyboard paints the layers in it, each over the ones before (cape_light.h has the meaning, the cell rule
 * and the reading; the format is described in corsair-protocol formats/cape/lighting.md and the pages it links).
 *
 * Only byte 0 of lght_NN.d, the effect type, is the same in every descriptor: the rest of the layout depends on the
 * family, which the size and the type tell apart (cape_light_desc_family):
 *  - 13 bytes, type 0-6, 8 or 9: a predefined effect, { effect, speed, colour type, direction, duration } and two
 *    colours (03 01 00 04 ... is the slow rainbow wave going down);
 *  - 37 bytes, type 7: a static colour (byte 18 is 1 and everything else zero);
 *  - 553 bytes, type 10-13: a custom effect (Solid, Gradient, Ripple, Wave): a header of 41 bytes and 128 colour
 *    samples, which are not the same fields at all.
 * cape_light_desc holds the first two, cape_light_custom the third; each parse function refuses the sizes of the
 * others. A static layer adds two files: lght_NN.k, the cells of the keys it covers (count u32, then one byte per
 * key, in iCUE's key order), and lght_NN.r, 256 RGBA entries of which a compact list from index 0 is in use: entry
 * i is the colour of the i-th cell of .k, a static layer has one colour, and the alpha is the opacity of the
 * layer. The custom effects have a .k and no .r; their colours are the samples of the descriptor. All of these
 * are handled as plain data here.
 */
#define CAPE_LIGHT_DESC_PREDEFINED  13
#define CAPE_LIGHT_DESC_STATIC      37
#define CAPE_LIGHT_DESC_MAX         CAPE_LIGHT_DESC_STATIC   // the larger of the two that cape_light_desc holds
#define CAPE_LIGHT_DESC_CUSTOM      553
#define CAPE_LIGHT_CUSTOM_HEADER    41
#define CAPE_LIGHT_SAMPLES          128
#define CAPE_COLORS_COUNT           256
#define CAPE_COLORS_SIZE            1024
#define CAPE_LAYERCOUNT_SIZE        4

typedef struct {
    size_t len;                                  // CAPE_LIGHT_DESC_PREDEFINED or CAPE_LIGHT_DESC_STATIC
    uint8_t effect;
    uint8_t speed;
    uint8_t color_type;
    uint8_t direction;
    uint8_t rest[CAPE_LIGHT_DESC_MAX - 4];       // bytes 4 and up, zero beyond len
} cape_light_desc;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} cape_rgba;

typedef enum {
    CAPE_LDESC_UNKNOWN = 0,   // another size, or a type that does not go with the size
    CAPE_LDESC_PREDEFINED,    // 13 bytes, type 0-6, 8 or 9
    CAPE_LDESC_STATIC,        // 37 bytes, type 7
    CAPE_LDESC_CUSTOM         // 553 bytes, type 10-13
} cape_ldesc_family;

// The family of a descriptor from its size and its type byte, and nothing else: the parse functions below check
// the rest of the layout. CAPE_LDESC_UNKNOWN for NULL and for an empty descriptor.
cape_ldesc_family cape_light_desc_family(const uint8_t* buf, size_t len);
// Name of an effect type as iCUE's enumeration calls it ("AdvancedWave"), "unknown" for a value outside it.
const char* cape_fx_name(uint8_t effect);

// A predefined (13 bytes) or static (37 bytes) descriptor. Returns CAPE_OK; CAPE_E_SIZE for any other size (the 553
// bytes of a custom effect go to cape_light_custom_parse), CAPE_E_MAGIC if the type does not go with the size.
int cape_light_desc_parse(const uint8_t* buf, size_t len, cape_light_desc* out);
// Returns the number of bytes written (in->len); CAPE_E_RANGE if len or effect are not those of a descriptor of
// these two families.
int cape_light_desc_build(const cape_light_desc* in, uint8_t* out, size_t cap);
// 13-byte descriptor of a predefined effect, colour type undefined, the rest zero.
void cape_light_desc_predefined(cape_light_desc* d, uint8_t effect, uint8_t speed, uint8_t direction);
// The 37-byte descriptor iCUE writes for the per-key static effect.
void cape_light_desc_static(cape_light_desc* d);

/*
 * A custom effect (corsair-protocol formats/cape/custom-effects.md): the 41-byte header of lght_NN.d and the colour curve of the layer, sampled
 * CAPE_LIGHT_SAMPLES times over its duration (sample j is at time j/128 of it, straight RGBA, transparent before
 * the first transition and after the last). Bytes that nothing is known about are kept, so that a descriptor
 * builds back exactly; every file seen has them zero. A writer that builds a layer from these fields would
 * drop them, so a layer where they are not zero should be kept as it was read.
 */
typedef struct {
    uint8_t effect;               // CAPE_FX_ADVANCEDSOLID to CAPE_FX_ADVANCEDWAVE
    uint8_t tail;                 // Wave and Ripple: length of the tail in lights; zero in Solid and Gradient
    uint8_t velocity;             // Wave and Ripple: lights per second
    uint8_t two_sided;            // Wave: 1 if it starts from both sides; zero elsewhere
    uint8_t unknown4;             // 1 in every Wave and Ripple seen, zero in Solid and Gradient; no option of iCUE changes it
    uint32_t angle;               // Wave: degrees (0, 90, 180 and 270 seen); zero elsewhere
    int32_t origin_x;             // where the animation starts, in the units of iCUE's drawing of the keyboard (6.7.3)
    int32_t origin_y;             //   (signed: an empty key list is -86, -13); zero in Solid and Gradient
    uint8_t start_on_key_press;   // the four flags are iCUE's executionHints of the layer
    uint8_t start_with_profile;
    uint8_t stop_on_key_press;
    uint8_t stop_on_key_release;
    uint16_t duration_100ms;      // duration of the cycle, in units of 0.1 s
    uint16_t stop_after_times;    // 'stop later', this many times; 0 when not set (presumed 16 bits wide: 0 and 4 seen)
    uint8_t reserved1[12];        // bytes 25 to 36
    uint8_t reserved2[3];         // bytes 38 to 40, after the number of samples
    cape_rgba samples[CAPE_LIGHT_SAMPLES];
} cape_light_custom;

// Returns CAPE_OK. CAPE_E_SIZE if len is not CAPE_LIGHT_DESC_CUSTOM or if the header does not say 128 samples,
// CAPE_E_MAGIC if the type is not 10 to 13.
int cape_light_custom_parse(const uint8_t* buf, size_t len, cape_light_custom* out);
// Returns CAPE_LIGHT_DESC_CUSTOM; CAPE_E_RANGE if the effect is not 10 to 13.
int cape_light_custom_build(const cape_light_custom* in, uint8_t* out, size_t cap);

// lght_NN.k. Returns the number of keys. With out == NULL only the count is returned.
int cape_keylist_parse(const uint8_t* buf, size_t len, uint8_t* out, size_t cap);
int cape_keylist_build(const uint8_t* keys, size_t n, uint8_t* out, size_t cap);

// lght_NN.r. Returns CAPE_OK / CAPE_COLORS_SIZE.
int cape_colors_parse(const uint8_t* buf, size_t len, cape_rgba out[CAPE_COLORS_COUNT]);
int cape_colors_build(const cape_rgba in[CAPE_COLORS_COUNT], uint8_t* out, size_t cap);

// lghtcnt.cnt. Returns CAPE_OK / CAPE_LAYERCOUNT_SIZE.
int cape_layercount_parse(const uint8_t* buf, size_t len, uint32_t* out);
int cape_layercount_build(uint32_t layers, uint8_t* out, size_t cap);

// Names of the lighting files of a layer: "lght_00.d", "lght_00.k", "lght_00.r" (ext is 'd', 'k' or 'r'; out
// must hold 10 bytes). Layers above 9 are refused because their numbering is not known.
int cape_light_filename(unsigned layer, char ext, char out[10]);

#endif  // CAPE_FORMAT_H
