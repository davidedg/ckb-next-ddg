#ifndef HWPICKERMAP_H
#define HWPICKERMAP_H

#include "keymap.h"

// The keyboard of iCUE's key pickers for a remap of a K95 RGB Platinum: the keys of
// the US ANSI K95P that iCUE offers (K95PKeyTable::icueKeyboard and icueMedia), with the labels of the device's layout; the key above
// Enter (bslash, the one iCUE writes for its "\") takes the label of the ISO key left of Enter (hash), which types the same. The other
// keys of the map (G1-G6, the profile, brightness and lock buttons, the lights) have no scan code, so KeyWidget neither draws nor
// picks them; the coordinates start at 0 and the map is as large as the keys shown.
namespace HwPickerMap {

KeyMap k95p(const KeyMap& device);

} // namespace HwPickerMap

#endif // HWPICKERMAP_H
