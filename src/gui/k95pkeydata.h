#ifndef K95PKEYDATA_H
#define K95PKEYDATA_H

/* The key indices of the files of the K95 RGB Platinum (PROFILE.MAP, PROFILE.DAT, the macro files): the first 152 entries of the
 * daemon's keymap (src/daemon/keymap.c), with the Linux input code each one sends (-1 none, -2 a Corsair key with no code of its
 * own). Plain C, for the GUI and for a test that compares it with the keymap. The name "hash" is the
 * key next to Enter of an ISO keyboard, which sends the same code as "bslash" of an ANSI one (KEY_BACKSLASH, 43). */

typedef struct {
    const char* name;   /* "": no key at this index */
    int evdev;
} k95p_key;

#define K95P_KEYS 152

static const k95p_key k95p_keys[K95P_KEYS] = {
    { "esc",           1 },  // 00 KEY_ESC
    { "f1",           59 },  // 01 KEY_F1
    { "f2",           60 },  // 02 KEY_F2
    { "f3",           61 },  // 03 KEY_F3
    { "f4",           62 },  // 04 KEY_F4
    { "f5",           63 },  // 05 KEY_F5
    { "f6",           64 },  // 06 KEY_F6
    { "f7",           65 },  // 07 KEY_F7
    { "f8",           66 },  // 08 KEY_F8
    { "f9",           67 },  // 09 KEY_F9
    { "f10",          68 },  // 0a KEY_F10
    { "f11",          87 },  // 0b KEY_F11
    { "grave",        41 },  // 0c KEY_GRAVE
    { "1",             2 },  // 0d KEY_1
    { "2",             3 },  // 0e KEY_2
    { "3",             4 },  // 0f KEY_3
    { "4",             5 },  // 10 KEY_4
    { "5",             6 },  // 11 KEY_5
    { "6",             7 },  // 12 KEY_6
    { "7",             8 },  // 13 KEY_7
    { "8",             9 },  // 14 KEY_8
    { "9",            10 },  // 15 KEY_9
    { "0",            11 },  // 16 KEY_0
    { "minus",        12 },  // 17 KEY_MINUS
    { "tab",          15 },  // 18 KEY_TAB
    { "q",            16 },  // 19 KEY_Q
    { "w",            17 },  // 1a KEY_W
    { "e",            18 },  // 1b KEY_E
    { "r",            19 },  // 1c KEY_R
    { "t",            20 },  // 1d KEY_T
    { "y",            21 },  // 1e KEY_Y
    { "u",            22 },  // 1f KEY_U
    { "i",            23 },  // 20 KEY_I
    { "o",            24 },  // 21 KEY_O
    { "p",            25 },  // 22 KEY_P
    { "lbrace",       26 },  // 23 KEY_LEFTBRACE
    { "caps",         58 },  // 24 KEY_CAPSLOCK
    { "a",            30 },  // 25 KEY_A
    { "s",            31 },  // 26 KEY_S
    { "d",            32 },  // 27 KEY_D
    { "f",            33 },  // 28 KEY_F
    { "g",            34 },  // 29 KEY_G
    { "h",            35 },  // 2a KEY_H
    { "j",            36 },  // 2b KEY_J
    { "k",            37 },  // 2c KEY_K
    { "l",            38 },  // 2d KEY_L
    { "colon",        39 },  // 2e KEY_SEMICOLON
    { "quote",        40 },  // 2f KEY_APOSTROPHE
    { "lshift",       42 },  // 30 KEY_LEFTSHIFT
    { "bslash_iso",   86 },  // 31 KEY_102ND
    { "z",            44 },  // 32 KEY_Z
    { "x",            45 },  // 33 KEY_X
    { "c",            46 },  // 34 KEY_C
    { "v",            47 },  // 35 KEY_V
    { "b",            48 },  // 36 KEY_B
    { "n",            49 },  // 37 KEY_N
    { "m",            50 },  // 38 KEY_M
    { "comma",        51 },  // 39 KEY_COMMA
    { "dot",          52 },  // 3a KEY_DOT
    { "slash",        53 },  // 3b KEY_SLASH
    { "lctrl",        29 },  // 3c KEY_LEFTCTRL
    { "lwin",        125 },  // 3d KEY_LEFTMETA
    { "lalt",         56 },  // 3e KEY_LEFTALT
    { "hanja",       123 },  // 3f KEY_HANJA
    { "space",        57 },  // 40 KEY_SPACE
    { "hangul",      122 },  // 41 KEY_HANGEUL
    { "katahira",     93 },  // 42 KEY_KATAKANAHIRAGANA
    { "ralt",        100 },  // 43 KEY_RIGHTALT
    { "rwin",        126 },  // 44 KEY_RIGHTMETA
    { "rmenu",       127 },  // 45 KEY_COMPOSE
    { "profswitch",   -2 },  // 46 KEY_CORSAIR
    { "light",        -2 },  // 47 KEY_CORSAIR
    { "f12",          88 },  // 48 KEY_F12
    { "prtscn",       99 },  // 49 KEY_SYSRQ
    { "scroll",       70 },  // 4a KEY_SCROLLLOCK
    { "pause",       119 },  // 4b KEY_PAUSE
    { "ins",         110 },  // 4c KEY_INSERT
    { "home",        102 },  // 4d KEY_HOME
    { "pgup",        104 },  // 4e KEY_PAGEUP
    { "rbrace",       27 },  // 4f KEY_RIGHTBRACE
    { "bslash",       43 },  // 50 KEY_BACKSLASH
    { "hash",         43 },  // 51 KEY_BACKSLASH_ISO
    { "enter",        28 },  // 52 KEY_ENTER
    { "ro",           89 },  // 53 KEY_RO
    { "equal",        13 },  // 54 KEY_EQUAL
    { "yen",         124 },  // 55 KEY_YEN
    { "bspace",       14 },  // 56 KEY_BACKSPACE
    { "del",         111 },  // 57 KEY_DELETE
    { "end",         107 },  // 58 KEY_END
    { "pgdn",        109 },  // 59 KEY_PAGEDOWN
    { "rshift",       54 },  // 5a KEY_RIGHTSHIFT
    { "rctrl",        97 },  // 5b KEY_RIGHTCTRL
    { "up",          103 },  // 5c KEY_UP
    { "left",        105 },  // 5d KEY_LEFT
    { "down",        108 },  // 5e KEY_DOWN
    { "right",       106 },  // 5f KEY_RIGHT
    { "lock",         -2 },  // 60 KEY_CORSAIR
    { "mute",        113 },  // 61 KEY_MUTE
    { "stop",        166 },  // 62 KEY_STOPCD
    { "prev",        165 },  // 63 KEY_PREVIOUSSONG
    { "play",        164 },  // 64 KEY_PLAYPAUSE
    { "next",        163 },  // 65 KEY_NEXTSONG
    { "numlock",      69 },  // 66 KEY_NUMLOCK
    { "numslash",     98 },  // 67 KEY_KPSLASH
    { "numstar",      55 },  // 68 KEY_KPASTERISK
    { "numminus",     74 },  // 69 KEY_KPMINUS
    { "numplus",      78 },  // 6a KEY_KPPLUS
    { "numenter",     96 },  // 6b KEY_KPENTER
    { "num7",         71 },  // 6c KEY_KP7
    { "num8",         72 },  // 6d KEY_KP8
    { "num9",         73 },  // 6e KEY_KP9
    { "",          -1 },  // 6f KEY_NONE
    { "num4",         75 },  // 70 KEY_KP4
    { "num5",         76 },  // 71 KEY_KP5
    { "num6",         77 },  // 72 KEY_KP6
    { "num1",         79 },  // 73 KEY_KP1
    { "num2",         80 },  // 74 KEY_KP2
    { "num3",         81 },  // 75 KEY_KP3
    { "num0",         82 },  // 76 KEY_KP0
    { "numdot",       83 },  // 77 KEY_KPDOT
    { "g1",           -2 },  // 78 KEY_CORSAIR
    { "g2",           -2 },  // 79 KEY_CORSAIR
    { "g3",           -2 },  // 7a KEY_CORSAIR
    { "g4",           -2 },  // 7b KEY_CORSAIR
    { "g5",           -2 },  // 7c KEY_CORSAIR
    { "g6",           -2 },  // 7d KEY_CORSAIR
    { "g7",           -2 },  // 7e KEY_CORSAIR
    { "g8",           -2 },  // 7f KEY_CORSAIR
    { "g9",           -2 },  // 80 KEY_CORSAIR
    { "g10",          -2 },  // 81 KEY_CORSAIR
    { "volup",       115 },  // 82 KEY_VOLUMEUP
    { "voldn",       114 },  // 83 KEY_VOLUMEDOWN
    { "mr",           -2 },  // 84 KEY_CORSAIR
    { "m1",           -2 },  // 85 KEY_CORSAIR
    { "m2",           -2 },  // 86 KEY_CORSAIR
    { "m3",           -2 },  // 87 KEY_CORSAIR
    { "g11",          -2 },  // 88 KEY_CORSAIR
    { "g12",          -2 },  // 89 KEY_CORSAIR
    { "g13",          -2 },  // 8a KEY_CORSAIR
    { "g14",          -2 },  // 8b KEY_CORSAIR
    { "g15",          -2 },  // 8c KEY_CORSAIR
    { "g16",          -2 },  // 8d KEY_CORSAIR
    { "g17",          -2 },  // 8e KEY_CORSAIR
    { "g18",          -2 },  // 8f KEY_CORSAIR
    { "muhenkan",     94 },  // 90 KEY_MUHENKAN
    { "henkan",       92 },  // 91 KEY_HENKAN
    { "fn",          464 },  // 92 KEY_FN
    { "",          -1 },  // 93 KEY_NONE
    { "",          -1 },  // 94 KEY_NONE
    { "",          -1 },  // 95 KEY_NONE
    { "",          -1 },  // 96 KEY_NONE
    { "",          -1 },  // 97 KEY_NONE
};

#endif
