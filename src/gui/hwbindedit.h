#ifndef HWBINDEDIT_H
#define HWBINDEDIT_H

#include <string>
#include <vector>
#include "hwbinding.h"

// What the editor of the hardware bindings of a K95 RGB Platinum (hwslot1) makes of an action and back: the kind it shows an
// action as, the events of a Shortcut, the rows of the event table of a Macro, and the checks a new or changed action must pass
// before it goes to the draft (the same ones the daemon makes before it writes anything, cape_binding_check()). Plain C++11.
namespace HwBindEdit {

constexpr unsigned EVENTS_MAX = 8191;       // events of one new or changed action: iCUE's largest macro file (16 390 bytes)
constexpr unsigned TEXT_CHARS_MAX = 1024;   // characters that can be typed or pasted into a new Text: iCUE's field (a line break is one)
// What iCUE's GUI takes for a hardware profile (corsair-protocol formats/cape/macro.md): a delay of a Macro, constant or random, up to 4095 ms; a
// Macro of up to 256 rows (its editor shows every event, delays of 0 too); a Text up to 99999 ms between characters; 'Imitate holding
// key' On press 0.1 to 16 777.2 s
constexpr unsigned MACRO_DELAY_MAX = 4095;  // ms
constexpr unsigned MACRO_ROWS_MAX = 256;
constexpr unsigned TEXT_DELAY_MAX = 99999;  // ms
constexpr unsigned IMITATE_MIN_MS = 100, IMITATE_MAX_MS = 16777200;
constexpr uint8_t START_PRESS = 0x00, START_RELEASE = 0x11;
constexpr uint8_t RUN_ONCE = 0x01, RUN_WHILE_PRESSED = 0x03, RUN_TOGGLE = 0x04;
constexpr uint8_t RUN_IMITATE_TOGGLE = 0x84;   // 'Imitate holding key' Toggle: a Keystroke run 84, repeat 0

// How an action is shown (only that: what can be written is check()'s): a remap by the place of its destination in iCUE's GUI (its
// Keyboard picker with the media keys, a mouse button, its Language keys), 'Imitate holding key' On press or Toggle, iCUE's
// Keystroke, a Macro, a Text; or KEPT for anything the editor cannot show as one of them in iCUE's form (it is kept as it is, and can
// only be replaced): a remap to a key iCUE does not offer, a Shortcut not in iCUE's form, a Macro with a delay above 4095 ms or not
// of 2 bytes, a Text out of shape, a time of 'Imitate' that is not a multiple of 100 ms (also iCUE's clamp c0 ff ff ff)
enum Kind { NATIVE, REMAP_KEYBOARD, REMAP_MOUSE, REMAP_LANGUAGE, IMITATE_PRESS, IMITATE_TOGGLE, SHORTCUT, MACRO, TEXT, KEPT };
Kind kindOf(const HwBinding::Action& a);

// Whether iCUE writes these values for a macro of this subtype (the daemon refuses others for a new or changed action); the
// Keystroke run 84 repeat 0 is 'Imitate holding key' Toggle, whose events kindOf() and check() look at
bool icueValues(uint8_t subtype, uint8_t start, uint8_t run, uint8_t repeat);

// 'Imitate holding key' (iCUE's Keyboard remap with that option): the key held and, On press, for how long; false if the action is
// not one in iCUE's form (a key of its keyboard, not a media key; On press 100 to 16 777 200 ms in steps of 100)
bool recognizeImitate(const HwBinding::Action& a, uint8_t& key, unsigned& ms, bool& toggle);
HwBinding::Action imitatePress(uint8_t key, unsigned ms);
HwBinding::Action imitateToggle(uint8_t key);

// A Shortcut: every key pressed in order, then released in reverse order, a delay of 0 between two events (iCUE's subtype 1)
std::vector<HwBinding::Event> shortcutEvents(const std::vector<uint8_t>& keys);
// Any keys in that shape (a Shortcut read from a slot is shown as it is, whatever its keys)
bool recognizeShortcut(const std::vector<HwBinding::Event>& ev, std::vector<uint8_t>& keys);
// iCUE's rules for a new or changed Shortcut (its Keystroke): at most three modifiers among Win, Ctrl, Alt and Shift,
// at most one other key, at least one key, pressed Win, Ctrl, Alt, Shift, then the key. shortcutKeys() makes keys so: a right
// modifier becomes the left one (iCUE writes the left ones only; a right one needs a Macro), a key twice counts once, and the order
// is iCUE's whatever the order of the clicks. False with why when the keys break a rule (keys are then left as they came).
bool shortcutKeys(std::vector<uint8_t>& keys, std::string& why);
// Whether keys, made by shortcutKeys(), are those of iCUE's rules and order
bool isIcueShortcut(const std::vector<uint8_t>& keys);

// One row of the event table of a Macro, one event each (also every delay of 0: iCUE's editor shows them)
struct Row {
    enum Type { PRESS, RELEASE, DELAY, RANDOM } type;
    unsigned value;                         // a key index, or ms (the lower bound of a random delay)
    unsigned max;                           // RANDOM: the upper bound
    Row() : type(PRESS), value(0), max(0) {}
    Row(Type t, unsigned v, unsigned m = 0) : type(t), value(v), max(m) {}
    bool operator==(const Row& o) const { return type == o.type && value == o.value && (type != RANDOM || max == o.max); }
};
// One row per event; false for an event that is none of the four (a delay of 2 bytes iCUE never writes, a0..bf, or an unknown one).
// A delay of 4 bytes is a DELAY row of its value (events() refuses it in a Macro above 4095 ms, with the reason)
bool rows(const std::vector<HwBinding::Event>& ev, std::vector<Row>& out);
// The events of the rows as iCUE's writer makes them: a delay of 0 between two key rows next to each other (so after Apply such a
// delay is a row of the table). False with why if they are not a Macro iCUE writes: a key that is not one, a delay above
// MACRO_DELAY_MAX, a random delay from more than to, more than MACRO_ROWS_MAX events once the delays of 0 are in, a key pressed
// twice, released unpressed or never released
bool events(const std::vector<Row>& rows, std::vector<HwBinding::Event>& out, std::string& why);
// The rows of a Macro as iCUE can have written it: every event but a delay of 0 between two key events (the daemon's count,
// cape_binding_macro_rows; what check() allows for an action already in the slot or copied from one)
unsigned savedRows(const std::vector<HwBinding::Event>& ev);

// Editing the table (the editor's drag and drop, context menu and buttons). A block is the rows [first, first + count).
// moveRows: the block goes before the row `to` of the order it had (0..size; size = at the end); returns where the block starts
// now, first if nothing moves (to inside the block or just after it, an empty block, a block or a to out of the table).
size_t moveRows(std::vector<Row>& rows, size_t first, size_t count, size_t to);
// insertRows: the copies go before the row `at` (clamped to size); returns where they start
size_t insertRows(std::vector<Row>& rows, size_t at, const std::vector<Row>& copies);

HwBinding::Action native();
HwBinding::Action remap(uint8_t dst);
HwBinding::Action macro(const std::vector<HwBinding::Event>& ev, uint8_t start, uint8_t run);
HwBinding::Action shortcut(const std::vector<uint8_t>& keys);   // the keys as shortcutKeys() makes them, when they keep the rules
HwBinding::Action text(const std::vector<HwBinding::Event>& ev);

// The checks of the whole slot against its base: only what differs from the base is checked, action by action (what is kept
// as it was read is written back as it is). No limit on the whole slot: one macro file per key always fits, and whether the files
// fit in the flash is for the daemon's writer to say
bool check(const HwBinding::Keys& draft, const HwBinding::Keys* base, std::string& why);

// The XKB names of a layout of ckb-next (KeyMap::getLayout()); false for one with no XKB layout
bool xkbNames(const std::string& ckbLayout, std::string& layout, std::string& variant);

// The keys a new Text may press with right Alt (AltGr): those that make a character with AltGr, alone or with Shift, on the Windows
// layout with the same name, as (Linux input code, Shift). A hardware action must type the same outside Linux,
// and XKB puts more on AltGr than Windows does (its "it" has the braces on AltGr+7 and AltGr+0 too, from the latin base it includes).
// false: no table for this layout yet, so no character on AltGr is allowed. A table is added only after checking the Windows layout.
// skip: keys that type something else on Windows even alone or with Shift, never pressed (the 102nd key of an ISO keyboard is "<>"
// in XKB's "us" but "\|" on Windows US and in a BIOS)
bool windowsAltGr(const std::string& ckbLayout, std::vector<std::pair<int, bool>>& keys, std::vector<int>* skip = nullptr);

} // namespace HwBindEdit

#endif
