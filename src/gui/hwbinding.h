#ifndef HWBINDING_H
#define HWBINDING_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include "hwlightingmodel.h"
#include <vector>

// The hardware bindings of a K95 RGB Platinum slot on the GUI's side of hwslot (the specification is the comment of
// src/daemon/cape_hwslot.h): the record the daemon sends for a slot,
// read strictly, and the words of the preparation of a save. Plain C++11, no Qt.
namespace HwBinding {

constexpr unsigned KEYS = 152;          // the key indices of the K95P files
constexpr unsigned LINE_BYTES = 512;      // bytes of a line of the protocol, the newline included
constexpr unsigned WORD_MAX = 400;
constexpr unsigned TEXT_MAX = 300;      // encoded bytes of the text of one name word
constexpr unsigned EVENT_BYTES = 128;   // bytes of the events of one e:/ev: word (cape_hwslot.h)
constexpr unsigned NAME_WORDS = 8;
constexpr unsigned NAME_UNITS = 114;    // UTF-16 units of a profile name the GUI writes (PROFILE.I keeps a terminator)
constexpr unsigned READ_NAME_UNITS = 115;   // and of one it reads: iCUE fills the whole area of a long name
constexpr unsigned TX_WORDS = 48;       // tx: words of one string at most
// 'Imitate holding key' on press: the longest interval a file can say (the delay c0 ff ff ff), and iCUE's longest (99 999 s)
constexpr unsigned long IMITATE_FILE_MAX = 16777215, IMITATE_MAX = 99999000;

enum Subtype : uint8_t { SUBTYPE_MACRO = 0, SUBTYPE_SHORTCUT = 1, SUBTYPE_TEXT = 3 };
enum Flag : unsigned { SHARED = 0x01, NONCANONICAL = 0x02, LOCKED = 0x04 };

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
    // 'Imitate holding key' on press longer than its file can say (its events: a press, the delay c0 ff ff ff, a release): the
    // interval of the slot's PROFILE.ZIP, more than IMITATE_FILE_MAX and at most IMITATE_MAX (im:); 0 for any other action
    unsigned long imitateMs = 0;
    // A Text (subtype 03): its string as iCUE keeps it (tx:, UTF-8, with its line ends and the characters its table leaves out
    // of the events); shown, not compared: a save keeps the slot's string for the same events
    std::string text;
    // The same for the keyboard and for the slot's PROFILE.ZIP: kind, destination, subtype, start, run, repeat, every event, and
    // the interval of an Imitate longer than its file can say
    bool sameAs(const Action& o) const;
    // Its events are a press, the delay c0 ff ff ff and a release: an Imitate on press at least as long as a file can say
    bool longestImitate() const;
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
    // OK: bindings that are a model (st:ok); EMPTY: an empty slot; READONLY: a slot that cannot be rewritten, no bindings known
    // (st:none)
    enum State { EMPTY, OK, READONLY } state = EMPTY;
    enum Light { STATIC, NONE, EFFECTS, UNKNOWN } light = NONE;
    // What is wrong with the slot's PROFILE.ZIP: the first four come with readonly, STALE (the files are not the ZIP's: a save
    // writes them again) does not
    enum Zip { ZIP_OK, ZIP_NONE, ZIP_UNKNOWN, ZIP_CORRUPT, ZIP_UNCOMPILABLE, ZIP_STALE } zip = ZIP_OK;
    std::string id;                     // "0" or "<guid>:<rev>", as :hwid prints it
    bool readonly = false;              // the slot cannot be rewritten (READONLY): its PROFILE.I, or its PROFILE.ZIP (zip)
    std::string readonlyWhy;            // UTF-8
    bool hasName = false;
    std::string name;                   // UTF-8
    bool hasWinlock = false;
    uint8_t winlock = 0;
    bool hasIndicators = false;
    std::array<uint8_t, 12> indicators{};
    unsigned layers = 0;              // derived from lighting, never a separate wire property
    HwLighting::Model lighting;
    bool activeEffects() const {
        for(uint32_t i = 0; i < lighting.data().count; ++i)
            if(lighting.data().layers[i].enabled && lighting.data().layers[i].effect.kind != CAPE_PROFILE_STATIC) return true;
        return false;
    }
    Keys keys;
};

// One line of an answer to "get :hwslot": a page, or an error
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
    HwLighting::Model lighting;        // light:model: the complete ordered list
    bool hasWinlock = false;            // wl: the Win Lock options to write (none: the slot's)
    uint8_t winlock = 0x01;
    bool hasIndicators = false;         // ind: the indicator colours to write (none: the slot's)
    std::array<uint8_t, 12> indicators{};
    bool keepBindings = true;
    Keys keys;                          // bind:model, with the interval of each Imitate longer than its file can say (im:)
};
// The words from begin to end, in the order and with the cuts the specification gives. Empty if the stage cannot be said (an event
// outside the keys, a name that is not UTF-8 or too long, a txn or mode out of range).
std::vector<std::string> stageWords(const Stage& s);
// "@<owner> hwslot" lines holding as many words each as fit
std::vector<std::string> packLines(unsigned owner, const std::vector<std::string>& words);
std::string checkLine(unsigned owner, const Stage& s);
std::string saveLine(unsigned owner, const Stage& s);
std::string abortLine(unsigned owner, const Stage& s);

// An id as :hwid prints it and the daemon reads it (cape_hwslot_parse_id): "0", or "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}:<rev>"
// with the GUID in upper case and rev in lower case hex without leading zeros
bool validId(const std::string& id);

// Percent-encoding and UTF-8, as the protocol has them
std::string encode(const std::string& bytes);
bool decode(const std::string& text, std::string& out);
bool validUtf8(const std::string& s, size_t* units = nullptr);
// A text cut into words of at most TEXT_MAX encoded bytes, as many whole characters each as fit
std::vector<std::string> cutText(const std::string& utf8);

} // namespace HwBinding

#endif
