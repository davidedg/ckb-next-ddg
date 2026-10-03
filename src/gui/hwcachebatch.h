#ifndef HWCACHEBATCH_H
#define HWCACHEBATCH_H

#include <array>
#include <string>
#include <vector>
#include "hwbinding.h"

// Collect one K95P cache load without publishing partially received data. The
// caller sends identityRequest(), waits for its modecount marker, then sends
// detailRequest() and waits for the second marker before applying result().
// With requestBindings() (hwslot1) a third phase follows: the record of
// each slot in the mask, one page of "get :hwbind" at a time, every page followed
// by its own marker. takeRequest() hands out each follow-up request exactly once.
class HwCacheBatch {
public:
    struct Mode {
        std::string guid;
        std::string revision;
        std::string name;
        std::string rgb;
        bool hasRgb = false;
        // hwslot1: the record of the slot (hasRecord false: the daemon has no cache of it, "error nocache")
        bool hasRecord = false;
        unsigned long gen = 0;
        HwBinding::Record record;
    };
    struct Result {
        unsigned modeCount = 0;
        std::string profileGuid;
        std::string profileRevision;
        std::string profileName;
        std::array<Mode, 3> modes;
    };
    enum State { Identity, Details, Bindings, Complete, Failed };

    // A refresh requests only modes in mask; an attach uses mask=7. rgbRequired
    // is a subset of mask: effect slots and a verified all-black save may be mute.
    HwCacheBatch(unsigned mask, unsigned rgbRequired, bool requireChangedIds);
    HwCacheBatch(unsigned mask, unsigned rgbRequired, bool requireChangedIds,
                 const Result& previous);
    std::string identityRequest(unsigned notify) const;
    std::string detailRequest(unsigned notify) const;
    // Before the first line only
    void requestBindings();
    bool bindings() const { return bindings_; }
    // The request of the phase or page that just began, once; empty otherwise
    std::string takeRequest(unsigned notify);
    bool accept(const std::string& line);
    unsigned mask() const { return mask_; }
    State state() const { return state_; }
    bool failureDrained() const { return state_ == Failed && failureDrained_; }
    const Result& result() const { return pending_; } // use only in Complete
    const std::string& error() const { return error_; }

private:
    void fail(const char* reason);
    void fail(const std::string& reason);
    void nextSlot(unsigned from);
    bool acceptPage(const std::string& line);
    bool pageMarker();
    unsigned mask_;
    unsigned rgbRequired_;
    bool requireChangedIds_;
    State state_ = Identity;
    Result previous_;
    Result pending_;
    std::string error_;
    bool profileIdSeen_ = false;
    bool profileNameSeen_ = false;
    unsigned modeIdsSeen_ = 0;
    unsigned modeNamesSeen_ = 0;
    unsigned modeRgbSeen_ = 0;
    bool failureDrained_ = false;
    bool requestReady_ = false;
    bool bindings_ = false;
    unsigned bindSlot_ = 0;         // 0..2 while in Bindings
    unsigned bindPage_ = 0;         // the page asked for
    unsigned bindPages_ = 0;
    unsigned long bindGen_ = 0;
    bool pageSeen_ = false;
    bool noCache_ = false;
    std::vector<std::string> bindWords_;
};

#endif
