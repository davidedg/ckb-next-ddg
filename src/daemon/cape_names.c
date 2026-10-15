#include "cape_names.h"

#include "cape_format.h"

#include <string.h>

typedef struct {
    const char* name;
    int code;
} name_code;

// The keys: iCUE's name and the key of the files, in the order of the keys; for a key with more than one name, the name it reads
// back as comes first (the Language keys the K95P does not have are written as Esc). The comments are the names of the daemon's
// keymap.
static const name_code keys[] = {
    { "Escape", 0x00 },                 // esc
    { "International6", 0x00 },         // written as Esc
    { "International7", 0x00 },         // written as Esc
    { "International8", 0x00 },         // written as Esc
    { "International9", 0x00 },         // written as Esc
    { "KeypadComma", 0x00 },            // written as Esc
    { "Lang3", 0x00 },                  // written as Esc
    { "Lang4", 0x00 },                  // written as Esc
    { "Lang5", 0x00 },                  // written as Esc
    { "Lang6", 0x00 },                  // written as Esc
    { "Lang7", 0x00 },                  // written as Esc
    { "Lang8", 0x00 },                  // written as Esc
    { "Lang9", 0x00 },                  // written as Esc
    { "F1", 0x01 },                     // f1
    { "F2", 0x02 },                     // f2
    { "F3", 0x03 },                     // f3
    { "F4", 0x04 },                     // f4
    { "F5", 0x05 },                     // f5
    { "F6", 0x06 },                     // f6
    { "F7", 0x07 },                     // f7
    { "F8", 0x08 },                     // f8
    { "F9", 0x09 },                     // f9
    { "F10", 0x0a },                    // f10
    { "F11", 0x0b },                    // f11
    { "GraveAccentAndTilde", 0x0c },    // grave
    { "1", 0x0d },                      // 1
    { "2", 0x0e },                      // 2
    { "3", 0x0f },                      // 3
    { "4", 0x10 },                      // 4
    { "5", 0x11 },                      // 5
    { "6", 0x12 },                      // 6
    { "7", 0x13 },                      // 7
    { "8", 0x14 },                      // 8
    { "9", 0x15 },                      // 9
    { "0", 0x16 },                      // 0
    { "MinusAndUnderscore", 0x17 },     // minus
    { "Tab", 0x18 },                    // tab
    { "Q", 0x19 },                      // q
    { "W", 0x1a },                      // w
    { "E", 0x1b },                      // e
    { "R", 0x1c },                      // r
    { "T", 0x1d },                      // t
    { "Y", 0x1e },                      // y
    { "U", 0x1f },                      // u
    { "I", 0x20 },                      // i
    { "O", 0x21 },                      // o
    { "P", 0x22 },                      // p
    { "BracketLeft", 0x23 },            // lbrace
    { "CapsLock", 0x24 },               // caps
    { "A", 0x25 },                      // a
    { "S", 0x26 },                      // s
    { "D", 0x27 },                      // d
    { "F", 0x28 },                      // f
    { "G", 0x29 },                      // g
    { "H", 0x2a },                      // h
    { "J", 0x2b },                      // j
    { "K", 0x2c },                      // k
    { "L", 0x2d },                      // l
    { "SemicolonAndColon", 0x2e },      // colon
    { "ApostropheAndDoubleQuote", 0x2f },// quote
    { "LeftShift", 0x30 },              // lshift
    { "NonUsBackslash", 0x31 },         // bslash_iso
    { "Z", 0x32 },                      // z
    { "X", 0x33 },                      // x
    { "C", 0x34 },                      // c
    { "V", 0x35 },                      // v
    { "B", 0x36 },                      // b
    { "N", 0x37 },                      // n
    { "M", 0x38 },                      // m
    { "CommaAndLessThan", 0x39 },       // comma
    { "PeriodAndBiggerThan", 0x3a },    // dot
    { "SlashAndQuestionMark", 0x3b },   // slash
    { "LeftCtrl", 0x3c },               // lctrl
    { "LeftGui", 0x3d },                // lwin
    { "LeftAlt", 0x3e },                // lalt
    { "Lang2", 0x3f },                  // hanja
    { "Space", 0x40 },                  // space
    { "Lang1", 0x41 },                  // hangul
    { "International2", 0x42 },         // katahira
    { "RightAlt", 0x43 },               // ralt
    { "RightGui", 0x44 },               // rwin
    { "Application", 0x45 },            // rmenu
    { "Key_Profile", 0x46 },            // profswitch
    { "Brightness", 0x47 },             // light
    { "F12", 0x48 },                    // f12
    { "PrintScreen", 0x49 },            // prtscn
    { "ScrollLock", 0x4a },             // scroll
    { "PauseBreak", 0x4b },             // pause
    { "Insert", 0x4c },                 // ins
    { "Home", 0x4d },                   // home
    { "PageUp", 0x4e },                 // pgup
    { "BracketRight", 0x4f },           // rbrace
    { "Backslash", 0x50 },              // bslash
    { "NonUsTilde", 0x51 },             // hash
    { "Enter", 0x52 },                  // enter
    { "International1", 0x53 },         // ro
    { "EqualsAndPlus", 0x54 },          // equal
    { "International3", 0x55 },         // yen
    { "Backspace", 0x56 },              // bspace
    { "Delete", 0x57 },                 // del
    { "End", 0x58 },                    // end
    { "PageDown", 0x59 },               // pgdn
    { "RightShift", 0x5a },             // rshift
    { "RightCtrl", 0x5b },              // rctrl
    { "UpArrow", 0x5c },                // up
    { "LeftArrow", 0x5d },              // left
    { "DownArrow", 0x5e },              // down
    { "RightArrow", 0x5f },             // right
    { "WinLock", 0x60 },                // lock
    { "Mute", 0x61 },                   // mute
    { "Stop", 0x62 },                   // stop
    { "ScanPreviousTrack", 0x63 },      // prev
    { "PlayPause", 0x64 },              // play
    { "ScanNextTrack", 0x65 },          // next
    { "NumLock", 0x66 },                // numlock
    { "KeypadSlash", 0x67 },            // numslash
    { "KeypadAsterisk", 0x68 },         // numstar
    { "KeypadMinus", 0x69 },            // numminus
    { "KeypadPlus", 0x6a },             // numplus
    { "KeypadEnter", 0x6b },            // numenter
    { "Keypad7", 0x6c },                // num7
    { "Keypad8", 0x6d },                // num8
    { "Keypad9", 0x6e },                // num9
    { "Keypad4", 0x70 },                // num4
    { "Keypad5", 0x71 },                // num5
    { "Keypad6", 0x72 },                // num6
    { "Keypad1", 0x73 },                // num1
    { "Keypad2", 0x74 },                // num2
    { "Keypad3", 0x75 },                // num3
    { "Keypad0", 0x76 },                // num0
    { "KeypadPeriodAndDelete", 0x77 },  // numdot
    { "G1", 0x78 },                     // g1
    { "G2", 0x79 },                     // g2
    { "G3", 0x7a },                     // g3
    { "G4", 0x7b },                     // g4
    { "G5", 0x7c },                     // g5
    { "G6", 0x7d },                     // g6
    { "VolumeUp", 0x82 },               // volup
    { "VolumeDown", 0x83 },             // voldn
    { "International5", 0x90 },         // muhenkan
    { "International4", 0x91 },         // henkan
    { "Mouse1", CAPE_KEY_MOUSE1 },      // the mouse buttons
    { "Mouse2", CAPE_KEY_MOUSE2 },
    { "Mouse3", CAPE_KEY_MOUSE3 },
    { "Mouse4", CAPE_KEY_MOUSE4 },
    { "Mouse5", CAPE_KEY_MOUSE5 },
};

// The lights: iCUE's name and the LED of the daemon's keymap, in the order of the LEDs (-1: no LED in the keymap)
static const name_code leds[] = {
    { "Lang1", -1 },                    // hangul
    { "Lang2", -1 },                    // hanja
    { "Escape", 0x00 },                 // esc
    { "GraveAccentAndTilde", 0x01 },    // grave
    { "Tab", 0x02 },                    // tab
    { "CapsLock", 0x03 },               // caps
    { "LeftShift", 0x04 },              // lshift
    { "LeftCtrl", 0x05 },               // lctrl
    { "F12", 0x06 },                    // f12
    { "EqualsAndPlus", 0x07 },          // equal
    { "WinLock", 0x08 },                // lock
    { "Keypad7", 0x09 },                // num7
    { "G3", 0x0a },                     // g1
    { "F1", 0x0c },                     // f1
    { "1", 0x0d },                      // 1
    { "Q", 0x0e },                      // q
    { "A", 0x0f },                      // a
    { "NonUsBackslash", 0x10 },         // bslash_iso
    { "LeftGui", 0x11 },                // lwin
    { "PrintScreen", 0x12 },            // prtscn
    { "International3", 0x13 },         // yen
    { "Mute", 0x14 },                   // mute
    { "Keypad8", 0x15 },                // num8
    { "G6", 0x16 },                     // g2
    { "F2", 0x18 },                     // f2
    { "2", 0x19 },                      // 2
    { "W", 0x1a },                      // w
    { "S", 0x1b },                      // s
    { "Z", 0x1c },                      // z
    { "LeftAlt", 0x1d },                // lalt
    { "ScrollLock", 0x1e },             // scroll
    { "Backspace", 0x1f },              // bspace
    { "Stop", 0x20 },                   // stop
    { "Keypad9", 0x21 },                // num9
    { "G9", 0x22 },                     // g3
    { "F3", 0x24 },                     // f3
    { "3", 0x25 },                      // 3
    { "E", 0x26 },                      // e
    { "D", 0x27 },                      // d
    { "X", 0x28 },                      // x
    { "International5", 0x29 },         // muhenkan
    { "PauseBreak", 0x2a },             // pause
    { "Delete", 0x2b },                 // del
    { "ScanPreviousTrack", 0x2c },      // prev
    { "G12", 0x2e },                    // g4
    { "F4", 0x30 },                     // f4
    { "4", 0x31 },                      // 4
    { "R", 0x32 },                      // r
    { "F", 0x33 },                      // f
    { "C", 0x34 },                      // c
    { "Space", 0x35 },                  // space
    { "Insert", 0x36 },                 // ins
    { "End", 0x37 },                    // end
    { "PlayPause", 0x38 },              // play
    { "Keypad4", 0x39 },                // num4
    { "G15", 0x3a },                    // g5
    { "F5", 0x3c },                     // f5
    { "5", 0x3d },                      // 5
    { "T", 0x3e },                      // t
    { "G", 0x3f },                      // g
    { "V", 0x40 },                      // v
    { "International4", 0x41 },         // henkan
    { "Home", 0x42 },                   // home
    { "PageDown", 0x43 },               // pgdn
    { "ScanNextTrack", 0x44 },          // next
    { "Keypad5", 0x45 },                // num5
    { "G18", 0x46 },                    // g6
    { "F6", 0x48 },                     // f6
    { "6", 0x49 },                      // 6
    { "Y", 0x4a },                      // y
    { "H", 0x4b },                      // h
    { "B", 0x4c },                      // b
    { "International2", 0x4d },         // katahira
    { "PageUp", 0x4e },                 // pgup
    { "RightShift", 0x4f },             // rshift
    { "NumLock", 0x50 },                // numlock
    { "Keypad6", 0x51 },                // num6
    { "F7", 0x54 },                     // f7
    { "7", 0x55 },                      // 7
    { "U", 0x56 },                      // u
    { "J", 0x57 },                      // j
    { "N", 0x58 },                      // n
    { "RightAlt", 0x59 },               // ralt
    { "BracketRight", 0x5a },           // rbrace
    { "RightCtrl", 0x5b },              // rctrl
    { "KeypadSlash", 0x5c },            // numslash
    { "Keypad1", 0x5d },                // num1
    { "F8", 0x60 },                     // f8
    { "8", 0x61 },                      // 8
    { "I", 0x62 },                      // i
    { "K", 0x63 },                      // k
    { "M", 0x64 },                      // m
    { "RightGui", 0x65 },               // rwin
    { "Backslash", 0x66 },              // bslash
    { "UpArrow", 0x67 },                // up
    { "KeypadAsterisk", 0x68 },         // numstar
    { "Keypad2", 0x69 },                // num2
    { "F9", 0x6c },                     // f9
    { "9", 0x6d },                      // 9
    { "O", 0x6e },                      // o
    { "L", 0x6f },                      // l
    { "CommaAndLessThan", 0x70 },       // comma
    { "Application", 0x71 },            // rmenu
    { "NonUsTilde", 0x72 },             // hash
    { "LeftArrow", 0x73 },              // left
    { "KeypadMinus", 0x74 },            // numminus
    { "Keypad3", 0x75 },                // num3
    { "F10", 0x78 },                    // f10
    { "0", 0x79 },                      // 0
    { "P", 0x7a },                      // p
    { "SemicolonAndColon", 0x7b },      // colon
    { "PeriodAndBiggerThan", 0x7c },    // dot
    { "Key_Profile", 0x7d },            // profswitch
    { "Enter", 0x7e },                  // enter
    { "DownArrow", 0x7f },              // down
    { "KeypadPlus", 0x80 },             // numplus
    { "Keypad0", 0x81 },                // num0
    { "F11", 0x84 },                    // f11
    { "MinusAndUnderscore", 0x85 },     // minus
    { "BracketLeft", 0x86 },            // lbrace
    { "ApostropheAndDoubleQuote", 0x87 },// quote
    { "SlashAndQuestionMark", 0x88 },   // slash
    { "Brightness", 0x89 },             // light
    { "International1", 0x8a },         // ro
    { "RightArrow", 0x8b },             // right
    { "KeypadEnter", 0x8c },            // numenter
    { "KeypadPeriodAndDelete", 0x8d },  // numdot
    { "Led_TopZone1", 0x90 },           // topbar1
    { "Led_TopZone2", 0x91 },           // topbar2
    { "Led_TopZone3", 0x92 },           // topbar3
    { "Led_TopZone6", 0x93 },           // topbar6
    { "Led_TopZone7", 0x94 },           // topbar7
    { "Led_TopZone8", 0x95 },           // topbar8
    { "Led_TopZone9", 0x96 },           // topbar9
    { "Led_TopZone10", 0x97 },          // topbar10
    { "Led_TopZone11", 0x98 },          // topbar11
    { "Led_TopZone12", 0x99 },          // topbar12
    { "Led_TopZone13", 0x9a },          // topbar13
    { "Led_TopZone14", 0x9b },          // topbar14
    { "Led_TopZone18", 0x9c },          // topbar18
    { "Led_TopZone19", 0x9d },          // topbar19
    { "Led_TopZone4", 0x9e },           // topbar4
    { "Led_TopZone15", 0x9f },          // topbar15
    { "Led_TopZone5", 0xa0 },           // topbar5
    { "Led_TopZone17", 0xa1 },          // topbar17
    { "Led_TopZone16", 0xa2 },          // topbar16
};

// The Media actions and the media keys they are written as
static const name_code media[] = {
    { "Mute", 0x61 },                   // mute
    { "Stop", 0x62 },                   // stop
    { "PrevTrack", 0x63 },              // prev
    { "PlayPause", 0x64 },              // play
    { "NextTrack", 0x65 },              // next
    { "VolumeUp", 0x82 },               // volup
    { "VolumeDown", 0x83 },             // voldn
};

// The lights a lighting layer can hold, on any of the five layouts, in the order iCUE lists them in (the order of its ids): the
// name, iCUE's id, the cell, and the layouts that have the light (bit N - 1 for layout N), as iCUE writes them
// (corsair-protocol formats/cape/lighting-cells.md).
typedef struct {
    const char* name;
    unsigned short id;
    unsigned char cell;
    unsigned char layouts;
} light;

static const light lights[] = {
    { "Escape",                    0x0001,   0, 0x1f },
    { "F1",                        0x0002,   8, 0x1f },
    { "F2",                        0x0003,  16, 0x1f },
    { "F3",                        0x0004,  24, 0x1f },
    { "F4",                        0x0005,  32, 0x1f },
    { "F5",                        0x0006,  40, 0x1f },
    { "F6",                        0x0007,  48, 0x1f },
    { "F7",                        0x0008,  56, 0x1f },
    { "F8",                        0x0009,  64, 0x1f },
    { "F9",                        0x000a,  72, 0x1f },
    { "F10",                       0x000b,  80, 0x1f },
    { "F11",                       0x000c,  88, 0x1f },
    { "GraveAccentAndTilde",       0x000d,   1, 0x1f },
    { "1",                         0x000e,   9, 0x1f },
    { "2",                         0x000f,  17, 0x1f },
    { "3",                         0x0010,  25, 0x1f },
    { "4",                         0x0011,  33, 0x1f },
    { "5",                         0x0012,  41, 0x1f },
    { "6",                         0x0013,  49, 0x1f },
    { "7",                         0x0014,  57, 0x1f },
    { "8",                         0x0015,  65, 0x1f },
    { "9",                         0x0016,  73, 0x1f },
    { "0",                         0x0017,  81, 0x1f },
    { "MinusAndUnderscore",        0x0018,  89, 0x1f },
    { "Tab",                       0x0019,   2, 0x1f },
    { "Q",                         0x001a,  10, 0x1f },
    { "W",                         0x001b,  18, 0x1f },
    { "E",                         0x001c,  26, 0x1f },
    { "R",                         0x001d,  34, 0x1f },
    { "T",                         0x001e,  42, 0x1f },
    { "Y",                         0x001f,  50, 0x1f },
    { "U",                         0x0020,  58, 0x1f },
    { "I",                         0x0021,  66, 0x1f },
    { "O",                         0x0022,  74, 0x1f },
    { "P",                         0x0023,  82, 0x1f },
    { "BracketLeft",               0x0024,  90, 0x1f },
    { "CapsLock",                  0x0025,   3, 0x1f },
    { "A",                         0x0026,  11, 0x1f },
    { "S",                         0x0027,  19, 0x1f },
    { "D",                         0x0028,  27, 0x1f },
    { "F",                         0x0029,  35, 0x1f },
    { "G",                         0x002a,  43, 0x1f },
    { "H",                         0x002b,  51, 0x1f },
    { "J",                         0x002c,  59, 0x1f },
    { "K",                         0x002d,  67, 0x1f },
    { "L",                         0x002e,  75, 0x1f },
    { "SemicolonAndColon",         0x002f,  83, 0x1f },
    { "ApostropheAndDoubleQuote",  0x0030,  91, 0x1f },
    { "LeftShift",                 0x0031,   4, 0x1f },
    { "NonUsBackslash",            0x0032,  12, 0x06 },
    { "Z",                         0x0033,  20, 0x1f },
    { "X",                         0x0034,  28, 0x1f },
    { "C",                         0x0035,  36, 0x1f },
    { "V",                         0x0036,  44, 0x1f },
    { "B",                         0x0037,  52, 0x1f },
    { "N",                         0x0038,  60, 0x1f },
    { "M",                         0x0039,  68, 0x1f },
    { "CommaAndLessThan",          0x003a,  76, 0x1f },
    { "PeriodAndBiggerThan",       0x003b,  84, 0x1f },
    { "SlashAndQuestionMark",      0x003c,  92, 0x1f },
    { "LeftCtrl",                  0x003d,   5, 0x1f },
    { "LeftGui",                   0x003e,  13, 0x1f },
    { "LeftAlt",                   0x003f,  21, 0x1f },
    { "Lang2",                     0x0040,  29, 0x10 },
    { "Space",                     0x0041,  37, 0x1f },
    { "Lang1",                     0x0042,  45, 0x10 },
    { "International2",            0x0043,  53, 0x08 },
    { "RightAlt",                  0x0044,  61, 0x1f },
    { "RightGui",                  0x0045,  69, 0x17 },
    { "Application",               0x0046,  77, 0x1f },
    { "F12",                       0x0049,   6, 0x1f },
    { "PrintScreen",               0x004b,  14, 0x1f },
    { "ScrollLock",                0x004c,  22, 0x1f },
    { "PauseBreak",                0x004d,  30, 0x1f },
    { "Insert",                    0x004e,  38, 0x1f },
    { "Home",                      0x004f,  46, 0x1f },
    { "PageUp",                    0x0050,  54, 0x1f },
    { "BracketRight",              0x0051,  62, 0x1f },
    { "Backslash",                 0x0052,  70, 0x11 },
    { "NonUsTilde",                0x0053,  78, 0x0e },
    { "Enter",                     0x0054,  86, 0x1f },
    { "International1",            0x0055,  94, 0x0c },
    { "EqualsAndPlus",             0x0056,   7, 0x1f },
    { "International3",            0x0057,  15, 0x08 },
    { "Backspace",                 0x0058,  23, 0x1f },
    { "Delete",                    0x0059,  31, 0x1f },
    { "End",                       0x005a,  39, 0x1f },
    { "PageDown",                  0x005b,  47, 0x1f },
    { "RightShift",                0x005c,  55, 0x1f },
    { "RightCtrl",                 0x005d,  63, 0x1f },
    { "UpArrow",                   0x005e,  71, 0x1f },
    { "LeftArrow",                 0x005f,  79, 0x1f },
    { "DownArrow",                 0x0060,  87, 0x1f },
    { "RightArrow",                0x0061,  95, 0x1f },
    { "Mute",                      0x0063, 104, 0x1f },
    { "Stop",                      0x0064, 112, 0x1f },
    { "ScanPreviousTrack",         0x0065, 120, 0x1f },
    { "PlayPause",                 0x0066, 128, 0x1f },
    { "ScanNextTrack",             0x0067, 136, 0x1f },
    { "NumLock",                   0x0068, 100, 0x1f },
    { "KeypadSlash",               0x0069, 108, 0x1f },
    { "KeypadAsterisk",            0x006a, 116, 0x1f },
    { "KeypadMinus",               0x006b, 124, 0x1f },
    { "KeypadPlus",                0x006c, 132, 0x1f },
    { "KeypadEnter",               0x006e, 140, 0x1f },
    { "Keypad7",                   0x006f,  97, 0x1f },
    { "Keypad8",                   0x0070, 105, 0x1f },
    { "Keypad9",                   0x0071, 113, 0x1f },
    { "Keypad4",                   0x0073, 129, 0x1f },
    { "Keypad5",                   0x0074, 137, 0x1f },
    { "Keypad6",                   0x0075, 101, 0x1f },
    { "Keypad1",                   0x0076, 109, 0x1f },
    { "Keypad2",                   0x0077, 117, 0x1f },
    { "Keypad3",                   0x0078, 125, 0x1f },
    { "Keypad0",                   0x0079, 133, 0x1f },
    { "KeypadPeriodAndDelete",     0x007a, 141, 0x1f },
    { "G3",                        0x007d,  98, 0x1f },
    { "G6",                        0x0080, 106, 0x1f },
    { "G9",                        0x0083, 114, 0x1f },
    { "G12",                       0x008c, 122, 0x1f },
    { "G15",                       0x008f, 130, 0x1f },
    { "G18",                       0x0092, 138, 0x1f },
    { "International5",            0x009a,  29, 0x08 },
    { "International4",            0x009b,  45, 0x08 },
    { "Led_TopZone1",              0x023c, 144, 0x1f },
    { "Led_TopZone2",              0x023d, 145, 0x1f },
    { "Led_TopZone3",              0x023e, 146, 0x1f },
    { "Led_TopZone4",              0x023f, 158, 0x1f },
    { "Led_TopZone5",              0x0240, 160, 0x1f },
    { "Led_TopZone6",              0x0241, 147, 0x1f },
    { "Led_TopZone7",              0x0242, 148, 0x1f },
    { "Led_TopZone8",              0x0243, 149, 0x1f },
    { "Led_TopZone9",              0x0244, 150, 0x1f },
    { "Led_TopZone10",             0x0245, 151, 0x1f },
    { "Led_TopZone11",             0x0246, 152, 0x1f },
    { "Led_TopZone12",             0x0247, 153, 0x1f },
    { "Led_TopZone13",             0x0248, 154, 0x1f },
    { "Led_TopZone14",             0x0249, 155, 0x1f },
    { "Led_TopZone15",             0x024a, 159, 0x1f },
    { "Led_TopZone16",             0x024b, 162, 0x1f },
    { "Led_TopZone17",             0x024c, 161, 0x1f },
    { "Led_TopZone18",             0x024d, 156, 0x1f },
    { "Led_TopZone19",             0x024e, 157, 0x1f },
};

// For each layout: how many colours iCUE writes for a layer over all keys (its lights and the three buttons)
static const unsigned all_colours[5] = { 137, 138, 139, 141, 139 };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int find(const name_code* table, size_t n, const char* name, size_t len){
    if(!name)
        return CAPE_PROFILE_NONE;
    for(size_t i = 0; i < n; i++)
        if(strlen(table[i].name) == len && !memcmp(table[i].name, name, len))
            return table[i].code;
    return CAPE_PROFILE_NONE;
}

static const char* find_name(const name_code* table, size_t n, int code){
    if(code < 0)
        return NULL;
    for(size_t i = 0; i < n; i++)
        if(table[i].code == code)
            return table[i].name;
    return NULL;
}

int cape_profile_key(const char* name, size_t len){
    return find(keys, COUNT(keys), name, len);
}

const char* cape_profile_key_name(int key){
    return find_name(keys, COUNT(keys), key);
}

int cape_profile_led(const char* name, size_t len){
    return find(leds, COUNT(leds), name, len);
}

const char* cape_profile_led_name(int led){
    return find_name(leds, COUNT(leds), led);
}

int cape_profile_media(const char* name, size_t len){
    return find(media, COUNT(media), name, len);
}

const char* cape_profile_media_name(int key){
    return find_name(media, COUNT(media), key);
}

static int layout_bit(int layout){
    return layout >= CAPE_PROFILE_LAYOUT_ANSI && layout <= CAPE_PROFILE_LAYOUT_KS ? 1 << (layout - 1) : 0;
}

int cape_profile_light(const char* name, size_t len, int layout, int* order){
    const int bit = layout_bit(layout);
    if(!name || !bit)
        return CAPE_PROFILE_NONE;
    for(size_t i = 0; i < COUNT(lights); i++)
        if(strlen(lights[i].name) == len && !memcmp(lights[i].name, name, len)){
            if(!(lights[i].layouts & bit))
                return CAPE_PROFILE_NONE;
            if(order)
                *order = (int)i;
            return lights[i].cell;
        }
    return CAPE_PROFILE_NONE;
}

const char* cape_profile_light_name(int cell, int layout){
    const int bit = layout_bit(layout);
    for(size_t i = 0; bit && i < COUNT(lights); i++)
        if(lights[i].cell == cell && (lights[i].layouts & bit))
            return lights[i].name;
    return NULL;
}

unsigned cape_profile_layout_all(int layout, unsigned char cells[CAPE_PROFILE_LAYER_MAX], unsigned* colours){
    const int bit = layout_bit(layout);
    unsigned n = 0;
    for(size_t i = 0; bit && i < COUNT(lights); i++)
        if((lights[i].layouts & bit) && n < CAPE_PROFILE_LAYER_MAX)
            cells[n++] = lights[i].cell;
    if(colours)
        *colours = bit ? all_colours[layout - 1] : 0;
    return n;
}

typedef struct {
    const char* name;
    int layout, x, y;
} geometry_point;
#include "cape_geometry.inc"

int cape_profile_anchor(const char* name, size_t len, int layout, int* x, int* y){
    if(!name) return 0;
    for(size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); ++i)
        if(geometry[i].layout == layout && strlen(geometry[i].name) == len && !memcmp(geometry[i].name, name, len)){
            if(x) *x = geometry[i].x;
            if(y) *y = geometry[i].y;
            return 1;
        }
    return 0;
}

unsigned cape_profile_anchors(int layout, int x[CAPE_PROFILE_LAYER_MAX + 3], int y[CAPE_PROFILE_LAYER_MAX + 3]){
    unsigned n = 0;
    for(size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); ++i)
        if(geometry[i].layout == layout && n < CAPE_PROFILE_LAYER_MAX + 3){
            if(x) x[n] = geometry[i].x;
            if(y) y[n] = geometry[i].y;
            ++n;
        }
    return n;
}
