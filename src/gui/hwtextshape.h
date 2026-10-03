#ifndef HWTEXTSHAPE_H
#define HWTEXTSHAPE_H

#include "hwbinding.h"

#include <string>
#include <vector>

// The events iCUE writes for a Text action of a K95 RGB Platinum (subtype 3), and back (corsair-protocol formats/cape/macro.md):
// every character is its key pressed and released, inside Shift (left) when it needs it; for a Text made here also inside AltGr
// (right Alt) for the third level of a layout and inside AltGr and Shift for the fourth. Between two key events there is always a delay event:
// 0 inside a character, the delay between characters after the last release of each one; none after the last event. Plain C++11.
namespace HwTextShape {

constexpr uint8_t LSHIFT = 0x30;    // key indices of the K95P files
constexpr uint8_t RALT = 0x43;
constexpr unsigned DELAY_MAX = 99999;   // ms between characters: iCUE's Text field; from 8192 ms a delay of 4 bytes

struct Stroke {
    uint8_t key = 0;                // the key index
    bool shift = false;
    bool altgr = false;
    bool operator==(const Stroke& o) const { return key == o.key && shift == o.shift && altgr == o.altgr; }
};

// The events of the strokes, with delayMs (<= DELAY_MAX) between characters. Empty if there are no strokes.
std::vector<HwBinding::Event> events(const std::vector<Stroke>& strokes, unsigned delayMs);
// The strokes and the delay of events that have exactly that shape; false otherwise (delay is 0 for one character).
bool recognize(const std::vector<HwBinding::Event>& ev, std::vector<Stroke>& strokes, unsigned& delayMs);

// iCUE's own table for a Text: the US layout, whatever the layout of the device and of the system (a character with no key in it,
// like é, is dropped by iCUE: false here). A newline is Enter (once for "\r\n").
bool icueUsStroke(char32_t c, Stroke& out);
std::vector<Stroke> icueUsStrokes(const std::u32string& text, std::u32string* dropped = nullptr);

// UTF-8 to UTF-32; false for invalid UTF-8
bool utf32(const std::string& utf8, std::u32string& out);

} // namespace HwTextShape

#endif
