#ifndef HWSLOTDRAFT_H
#define HWSLOTDRAFT_H

#include <string>
#include "hwbinding.h"

// The draft of an on-board slot of a K95 RGB Platinum in the GUI (hwslot1): what the user is making the slot become, on top of
// the base, the record the daemon last read from the keyboard. The name and the colours of the draft are those of the mode of
// the hardware profile (KbMode); this holds its bindings and what ties the draft to its base. A new record of the slot fills
// the base; it replaces the draft only when the draft has no changes of its own, or when it is the read-back of our own save.
// Plain C++11, no Qt.
namespace HwSlotDraft {

struct Draft {
    std::string base;                   // the id of the record the draft was made on ("" before the first one)
    bool modified = false;              // the user changed the name, the colours, the bindings or the performance settings since the base
    bool conflict = false;              // modified, and a record with another id came: the user decides (reload or keep)
    bool hasBindings = false;           // keys hold the bindings of the draft (a slot whose record is OK)
    HwBinding::Keys keys;
    // The performance settings: known to the draft as the keys are (a record's, copied by replace(), or the user's); a value that
    // is not known (a record without it, a draft of the settings of an older GUI) is the base's
    bool hasWinlock = false;
    uint8_t winlock = HwBinding::DEFAULT_WINLOCK;
    bool hasIndicators = false;
    std::array<uint8_t, 12> indicators = HwBinding::DEFAULT_INDICATORS;
    bool recreate = false;              // "Recreate the bindings from scratch" on a RAW or BROKEN slot
    bool replaceLights = false;         // "Replace with static colours" on a slot with effects
};

// The performance settings of a slot: the Win Lock options (byte 1 of PROFILE.DAT) and the indicator colours (profile, brightness,
// lock on, lock off: RGB each)
struct Perf {
    uint8_t winlock = HwBinding::DEFAULT_WINLOCK;
    std::array<uint8_t, 12> indicators = HwBinding::DEFAULT_INDICATORS;
    bool operator==(const Perf& o) const { return winlock == o.winlock && indicators == o.indicators; }
    bool operator!=(const Perf& o) const { return !(*this == o); }
};
// A record's (NULL, or a value it does not have: iCUE's defaults, as the daemon takes them)
Perf basePerf(const HwBinding::Record* record);
// The draft's: its values where it knows them, else fallback's (the base it was made on), else iCUE's defaults
Perf effectivePerf(const Draft& d, const HwBinding::Record* fallback);

enum Apply {
    REPLACE,                            // the draft becomes the new base (nothing of its own to lose)
    KEEP,                               // the same base: the draft stays
    CONFLICT                            // modified, and the slot changed under it
};

// What a new record with id newId does to the draft; committing: it is the read-back of our own save of this slot
Apply decide(const Draft& d, const std::string& newId, bool committing);
// The draft made the base of the record (REPLACE, or "reload" after a conflict)
void replace(Draft& d, const HwBinding::Record& record, bool hasRecord);
// "Keep my changes" after a conflict: they now apply to the new base
void keep(Draft& d, const std::string& newId);

// For the settings of the GUI: one line of words, and back; a line that does not read gives a clean draft and false
std::string serialize(const Draft& d);
bool deserialize(const std::string& line, Draft& d);

} // namespace HwSlotDraft

#endif
