#include "k95pkeytable.h"

#include "k95pkeydata.h"

#include <cstdio>

namespace K95PKeyTable {

namespace {
const int KEY_BACKSLASH_CODE = 43, KEY_102ND_CODE = 86;
const unsigned HASH = 0x51, BSLASH = 0x50;
} // namespace

std::string label(unsigned index){
    char buf[8];
    if(index >= MOUSE_FIRST && index <= MOUSE_LAST){
        std::snprintf(buf, sizeof(buf), "mouse%u", index - MOUSE_FIRST + 1);
        return buf;
    }
    if(index >= KEYS)
        return std::string();
    if(k95p_keys[index].name[0])
        return k95p_keys[index].name;
    std::snprintf(buf, sizeof(buf), "#%02x", index);
    return buf;
}

int indexOf(const std::string& name){
    if(name.size() == 3 && name[0] == '#'){
        unsigned v = 0;
        for(size_t i = 1; i < 3; ++i){
            const char c = name[i];
            if(c >= '0' && c <= '9') v = v * 16 + static_cast<unsigned>(c - '0');
            else if(c >= 'a' && c <= 'f') v = v * 16 + static_cast<unsigned>(c - 'a' + 10);
            else return -1;
        }
        return v < KEYS ? static_cast<int>(v) : -1;
    }
    for(unsigned i = 0; i < KEYS; ++i)
        if(k95p_keys[i].name[0] && name == k95p_keys[i].name)
            return static_cast<int>(i);
    for(unsigned m = MOUSE_FIRST; m <= MOUSE_LAST; ++m)
        if(name == label(m))
            return static_cast<int>(m);
    return -1;
}

int evdev(unsigned index){
    return index < KEYS ? k95p_keys[index].evdev : -1;
}

int indexOfEvdev(int code, bool iso){
    if(code == KEY_BACKSLASH_CODE)
        return static_cast<int>(iso ? HASH : BSLASH);
    if(code == KEY_102ND_CODE && !iso)
        return -1;      // an ANSI keyboard has no key between left Shift and Z
    if(code < 0)
        return -1;
    for(unsigned i = 0; i < KEYS; ++i)
        if(k95p_keys[i].name[0] && k95p_keys[i].evdev == code)
            return static_cast<int>(i);
    return -1;
}

namespace {
// By name, as the keymap names them
const char* const ICUE_KEYBOARD[] = {
    "esc", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "prtscn", "scroll", "pause",
    "grave", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "minus", "equal", "bspace", "ins", "home", "pgup",
    "numlock", "numslash", "numstar", "numminus", "tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "lbrace", "rbrace",
    "bslash", "del", "end", "pgdn", "num7", "num8", "num9", "numplus", "caps", "a", "s", "d", "f", "g", "h", "j", "k", "l",
    "colon", "quote", "enter", "num4", "num5", "num6", "lshift", "z", "x", "c", "v", "b", "n", "m", "comma", "dot", "slash",
    "rshift", "up", "num1", "num2", "num3", "numenter", "lctrl", "lwin", "lalt", "space", "ralt", "rwin", "rmenu", "rctrl",
    "left", "down", "right", "num0", "numdot",
};
const char* const ICUE_MEDIA[] = { "mute", "volup", "voldn", "stop", "prev", "play", "next" };
const char* const ICUE_LANGUAGE[] = { "hangul", "hanja", "ro", "katahira", "yen", "henkan", "muhenkan", "hash", "bslash_iso" };

template<size_t N> bool inList(const char* const (&list)[N], unsigned index){
    for(const char* name : list)
        if(indexOf(name) == int(index)) return true;
    return false;
}
} // namespace

bool icueKeyboard(unsigned index){ return index < KEYS && inList(ICUE_KEYBOARD, index); }
bool icueMedia(unsigned index){ return index < KEYS && inList(ICUE_MEDIA, index); }
bool icueLanguage(unsigned index){ return index < KEYS && inList(ICUE_LANGUAGE, index); }
bool isIcueDestination(unsigned index){
    return (index >= MOUSE_FIRST && index <= MOUSE_LAST) || icueKeyboard(index) || icueMedia(index) || icueLanguage(index);
}

} // namespace K95PKeyTable
