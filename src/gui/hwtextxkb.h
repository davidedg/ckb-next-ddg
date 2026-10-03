#ifndef HWTEXTXKB_H
#define HWTEXTXKB_H

#include "hwtextshape.h"

#include <memory>
#include <utility>
#include <string>
#include <vector>

// A new Text for a K95 RGB Platinum typed with the layout of the device: every character becomes the
// key of the K95P files that makes it in an XKB keymap, pressed alone, with left Shift, with right Alt (AltGr) or with both, the way
// the keymap itself says when those keys are pressed (so a layout whose right Alt is not AltGr, like "us", has no third level).
// Keys of the main block first, the keypad only for what nothing else makes; dead keys make nothing. Linux, libxkbcommon.
class HwTextXkb {
public:
    // A keymap from the XKB names (rules "evdev", model "pc105"), or from the text of a keymap (the tests' fixed ones). iso: the
    // physical layout of the keyboard (KEY_BACKSLASH is "hash" on ISO, "bslash" on ANSI).
    // altgr: the keys (Linux input code, Shift) allowed with right Alt, those of the Windows layout (HwBindEdit::windowsAltGr());
    // nullptr: every one of the keymap. skip: keys never pressed (they type something else on Windows). A character that only a key
    // left out makes is "not on Windows".
    static std::unique_ptr<HwTextXkb> fromNames(const std::string& layout, const std::string& variant, bool iso,
                                                const std::vector<std::pair<int, bool>>* altgr = nullptr,
                                                const std::vector<int>* skip = nullptr);
    static std::unique_ptr<HwTextXkb> fromString(const std::string& keymap, bool iso,
                                                 const std::vector<std::pair<int, bool>>* altgr = nullptr,
                                                 const std::vector<int>* skip = nullptr);
    ~HwTextXkb();

    // The strokes of a text; the characters that no key makes go to rejected, those that only keys left out of altgr make go to
    // notOnWindows (or to rejected when it is nullptr), each once, in order. A newline is Enter ("\r\n" once).
    std::vector<HwTextShape::Stroke> strokes(const std::u32string& text, std::u32string* rejected,
                                             std::u32string* notOnWindows = nullptr) const;
    // The character a stroke makes, 0 if none (for the text shown for events that have the shape of a Text)
    char32_t character(const HwTextShape::Stroke& s) const;

private:
    struct Impl;
    explicit HwTextXkb(Impl* impl);
    std::unique_ptr<Impl> impl_;
};

#endif
