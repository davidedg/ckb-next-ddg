#ifndef K95PSTAGE_H
#define K95PSTAGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "keymap.h"
#include <QString>
#include "hwbinding.h"
#include "hwslotdraft.h"

// The protocol and the file formats are described in the K95 RGB Platinum pages of https://github.com/davidedg/corsair-protocol
// (devices/k95p.md, formats/cape/); the comments of this code refer to them as "corsair-protocol <path>".

// The preparation of the save of one slot of a K95 RGB Platinum (hwslot): what the slot is to become, as the lines of
// its transaction. Built from the complete base, layer/binding/performance draft and mode name.
// Nothing of the GUI or device changes while it is built.
struct K95PStage {
    HwBinding::Stage stage;
    std::vector<std::string> prepare;       // "@N hwslot" lines, begin to end
    std::string check, save, abort;         // "@N hwslot check:/save:/abort:" lines
};

// mode 1..3; owner is the notification node; txn is nonzero. The model and bindings are kept
// when equal to the base. Name and performance words are emitted only when changed.
bool makeK95PStage(const KeyMap& map, const QString& name, const QString& baseName,
                   const HwBinding::Record& base, const HwSlotDraft::Draft& draft,
                   unsigned mode, unsigned owner, uint32_t txn, K95PStage& out, std::string& error);

#endif
