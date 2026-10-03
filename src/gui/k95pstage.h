#ifndef K95PSTAGE_H
#define K95PSTAGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "colormap.h"
#include "hwbinding.h"
#include "hwslotdraft.h"

// The protocol and the file formats are described in the K95 RGB Platinum pages of https://github.com/davidedg/corsair-protocol
// (devices/k95p.md, formats/cape/); the comments of this code refer to them as "corsair-protocol <path>".

// The preparation of the save of one slot of a K95 RGB Platinum (hwslot1): what the slot is to become, as the lines of
// its transaction. Built from the base (the record the edit was made on), the draft (the bindings) and the mode of the hardware
// profile (the name and colours); nothing of the GUI or of the device changes while it is built.
struct K95PStage {
    HwBinding::Stage stage;
    std::vector<std::string> prepare;       // "@N hwslot" lines, begin to end
    std::string check, save, abort;         // "@N hwslot check:/save:/abort:" lines
    bool pic = false;                        // the lighting becomes static layers of the mode's colours (else it is kept)
    unsigned colours = 0;                    // pic: distinct colours that are not black
    bool invisibleKeys = false;              // pic: colours on keys a lighting file cannot hold
    bool rgbRequired = false;                // the read-back of a save of it has lighting to answer (:hwrgb)
};

// mode 1..3; owner the notification node of the transaction; txn not 0. baseName the name the mode got from the base, and
// lightsChanged whether the mode's colours differ from the base's (both as Kb knows them). The name goes when it differs from
// baseName (always for an empty slot); the colours when they changed on static lighting (or none), or when the draft replaces
// the lighting effects; the bindings when they differ from the base's, or when they are recreated.
bool makeK95PStage(const KeyMap& map, const QColorMap& colours, const QString& name, const QString& baseName,
                   bool lightsChanged, const HwBinding::Record& base, const HwSlotDraft::Draft& draft,
                   unsigned mode, unsigned owner, uint32_t txn, K95PStage& out, std::string& error);

#endif
