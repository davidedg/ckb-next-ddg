#ifndef K95PKEYTABLE_H
#define K95PKEYTABLE_H

#include <string>

// The key indices of the K95 RGB Platinum files as the GUI shows and converts them (the data is k95pkeydata.h, the first 152
// entries of the daemon's keymap). Plain C++11, no Qt.
namespace K95PKeyTable {

constexpr unsigned KEYS = 152;
constexpr unsigned MOUSE_FIRST = 0xc8;   // a remap may send mouse1..mouse5
constexpr unsigned MOUSE_LAST = 0xcc;

// The name of an index ("#6f" for one with no name), or of a remap destination (also "mouse1".."mouse5"); "" if neither
std::string label(unsigned index);
// The index of a name as label() gives it (also "#6f", in lowercase hex, for any index below KEYS), -1 if none
int indexOf(const std::string& name);
// The Linux input code of an index: -1 none, -2 a Corsair key without one
int evdev(unsigned index);
// The index of a Linux input code on an ISO or an ANSI keyboard (KEY_BACKSLASH is "hash" on ISO, "bslash" on ANSI; KEY_102ND,
// "bslash_iso", is only on ISO), -1 if none
int indexOfEvdev(int code, bool iso);

// The keys iCUE's GUI offers for a hardware profile (as the daemon's cape_binding_icue_sets): its
// Keyboard picker (the US ANSI keys of the K95P without G1-G6, the profile, brightness and lock buttons), its media keys, the 9
// Language keys it writes on a real key; a destination of a remap is one of these or a mouse button
bool icueKeyboard(unsigned index);
bool icueMedia(unsigned index);
bool icueLanguage(unsigned index);
bool isIcueDestination(unsigned index);

} // namespace K95PKeyTable

#endif
