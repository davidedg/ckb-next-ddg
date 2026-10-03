#ifndef HWBINDING_H
#define HWBINDING_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The hardware bindings of a K95 RGB Platinum slot on the GUI's side of hwslot1 (the specification is the comment of
// src/daemon/cape_hwslot.h): the record the daemon sends for a slot,
// read strictly, and the words of the preparation of a save. Plain C++11, no Qt.
namespace HwBinding {

constexpr unsigned KEYS = 152;          // the key indices of the K95P files
constexpr unsigned LINE_BYTES = 512;      // bytes of a line of the protocol, the newline included
constexpr unsigned WORD_MAX = 400;
constexpr unsigned TEXT_MAX = 300;      // encoded bytes of the text of one name word
constexpr unsigned EVENT_BYTES = 128;   // bytes of the events of one e:/ev: word (cape_hwslot.h)
constexpr unsigned NAME_WORDS = 8;
constexpr unsigned NAME_UNITS = 114;    // UTF-16 units of a profile name

enum Subtype : uint8_t { MACRO = 0, SHORTCUT = 1, TEXT = 3 };
enum Flag : unsigned { SHARED = 0x01, NON_ICUE = 0x02, LOCKED = 0x04 };

// One event of a macro file, as in the file: 2 bytes, or 4 when bit 6 of the first byte is set (corsair-protocol formats/cape/macro.md).
// The bytes past its length are zero (the functions below make them so); two events are the same by their own bytes.
struct Event {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    unsigned size() const { return (b0 & 0x40) ? 4u : 2u; }
    bool operator==(const Event& o) const { return b0 == o.b0 && b1 == o.b1 && (size() == 2 || (b2 == o.b2 && b3 == o.b3)); }
    bool operator!=(const Event& o) const { return !(*this == o); }
};

// The kinds of events (cape_format.h): a press, a release, a delay of 13 bits (iCUE below 8192 ms), of 24 bits (iCUE from 8192 ms),
// a random delay (two bounds of 12 bits), a 2-byte delay iCUE reads but never writes (a0..bf), anything else
enum EventKind { EV_UNKNOWN, EV_PRESS, EV_RELEASE, EV_DELAY, EV_LONG, EV_RANDOM, EV_ODD_SHORT };
EventKind eventKind(const Event& e);
constexpr unsigned DELAY_SHORT_MAX = 0x1fff, DELAY_LONG_MAX = 0xffffff, RANDOM_MAX = 0xfff;
Event press(uint8_t key);
Event release(uint8_t key);
// A delay as iCUE writes it: 2 bytes below 8192 ms, 4 from there (ms <= DELAY_LONG_MAX)
Event delay(unsigned ms);
// A random delay (min <= max <= RANDOM_MAX)
Event randomDelay(unsigned min, unsigned max);
// The milliseconds of EV_DELAY, EV_LONG and EV_ODD_SHORT (13 bits, as iCUE's reader); 0 for anything else
unsigned delayMs(const Event& e);
unsigned randomMin(const Event& e);
unsigned randomMax(const Event& e);

struct Action {
    enum Kind { NATIVE, REMAP, MACRO } kind = NATIVE;
    uint8_t dst = 0;                    // REMAP
    uint8_t subtype = 0, start = 0, run = 0, repeat = 0;
    std::vector<Event> events;          // MACRO
    std::string file;                   // the name of its file in the slot ("" for a new one): shown, not compared
    unsigned flags = 0;                 // Flag, from the record: not compared
    // The same for the keyboard: kind, destination, subtype, start, run, repeat and every event
    bool sameAs(const Action& o) const;
};

typedef std::array<Action, KEYS> Keys;

// Whether two sets of bindings do the same (the keys: the Win Lock options are not part of the bindings)
bool sameBindings(const Keys& a, const Keys& b);

// iCUE's performance settings of a new profile: Win Lock disables the Windows key only; the indicator colours profile ff0000,
// brightness ffffff, lock on 00ffff, lock off ff0000 (corsair-protocol formats/cape/profile-dat.md, profile-i.md)
const uint8_t DEFAULT_WINLOCK = 0x01;
const std::array<uint8_t, 12> DEFAULT_INDICATORS = {{0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00}};

// The record of a slot (section 1)
struct Record {
    enum State { EMPTY, OK, RAW, BROKEN } state = EMPTY;
    enum Light { STATIC, NONE, EFFECTS, UNKNOWN } light = NONE;
    std::string id;                     // "0" or "<guid>:<rev>", as :hwid prints it
    std::string reason;                 // RAW and BROKEN, UTF-8
    bool readonly = false;              // the slot cannot be rewritten at all (PROFILE.I), whatever its bindings are
    std::string readonlyWhy;            // UTF-8
    bool hasName = false;
    std::string name;                   // UTF-8
    bool hasWinlock = false;
    uint8_t winlock = 0;
    bool hasIndicators = false;
    std::array<uint8_t, 12> indicators{};
    unsigned layers = 0;
    Keys keys;
};

// One line of an answer to "get :hwbind": a page, or an error
struct Page {
    unsigned mode = 0;
    unsigned long gen = 0;
    bool error = false;
    std::string why;                    // for an error: nocache or range
    unsigned page = 0, pages = 0;
    std::vector<std::string> words;
};
bool parsePage(const std::string& line, Page& out, std::string& err);
// The words of pages 1..pages, in order, as a record
bool parseRecord(const std::vector<std::string>& words, Record& out, std::string& err);

// The preparation of a save (section 2)
struct Stage {
    std::string txn;                    // 8 lowercase hex digits, not all zeros
    unsigned mode = 0;                  // 1..3
    std::string base;                   // the id of the slot the edit was made on
    bool hasName = false;
    std::string name;                   // UTF-8, 1..114 UTF-16 units
    bool keepLight = true;
    std::map<std::string, uint32_t> rgb;    // light:pic: the colour of each LED (0xrrggbb; black ones are left out)
    bool hasWinlock = false;            // wl: the Win Lock options to write (none: the slot's)
    uint8_t winlock = 0x01;
    bool hasIndicators = false;         // ind: the indicator colours to write (none: the slot's)
    std::array<uint8_t, 12> indicators{};
    bool keepBindings = true;
    bool recreate = false;
    Keys keys;
};
// The words from begin to end, in the order and with the cuts the specification gives. Empty if the stage cannot be said (an event
// outside the keys, a name that is not UTF-8 or too long, a txn or mode out of range).
std::vector<std::string> stageWords(const Stage& s);
// "@<owner> hwslot" lines holding as many words each as fit
std::vector<std::string> packLines(unsigned owner, const std::vector<std::string>& words);
std::string checkLine(unsigned owner, const Stage& s);
std::string saveLine(unsigned owner, const Stage& s);
std::string abortLine(unsigned owner, const Stage& s);

// Percent-encoding and UTF-8, as the protocol has them
std::string encode(const std::string& bytes);
bool decode(const std::string& text, std::string& out);
bool validUtf8(const std::string& s, size_t* units = nullptr);
// A text cut into words of at most TEXT_MAX encoded bytes, as many whole characters each as fit
std::vector<std::string> cutText(const std::string& utf8);

} // namespace HwBinding

#endif
