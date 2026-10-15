#include "k95pstage.h"

#include <cstdio>

bool makeK95PStage(const KeyMap& map, const QString& name, const QString& baseName,
                   const HwBinding::Record& base, const HwSlotDraft::Draft& draft,
                   unsigned mode, unsigned owner, uint32_t txn, K95PStage& out, std::string& error){
    error.clear();
    K95PStage result;
    HwBinding::Stage& s = result.stage;
    if(map.model() != KeyMap::K95P || mode < 1 || mode > 3 || owner < 1 || owner > 9 || txn == 0){
        error = "invalid K95P slot"; return false;
    }
    if(draft.conflict){ error = "the slot changed on the keyboard: reload it or keep the changes first"; return false; }
    if(draft.base.empty()){ error = "the slot was not read"; return false; }
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", unsigned(txn));
    s.txn = hex;
    s.mode = mode;
    s.base = draft.base;

    // The name: when it is not the one the slot has (an empty slot has none), compared as a mode holds it (KbMode::name():
    // trimmed, "Unnamed" for nothing)
    const QString held = baseName.trimmed().isEmpty() ? QString("Unnamed") : baseName.trimmed();
    if(base.id == "0" || name != held){
        if(name.length() > int(HwBinding::NAME_UNITS)){
            error = "the name is longer than " + std::to_string(HwBinding::NAME_UNITS) + " characters"; return false;
        }
        const QByteArray utf8 = name.toUtf8();
        s.hasName = true;
        s.name.assign(utf8.constData(), size_t(utf8.size()));
    }

    // The performance settings: wl: and ind: when the draft's are not the slot's. Bits 4 to 7 of the Win Lock options have no place
    // in iCUE's model, which the slot's PROFILE.ZIP holds: they are never written (imported values may have them)
    HwSlotDraft::Perf want = HwSlotDraft::effectivePerf(draft, &base);
    want.winlock &= 0x0f;
    const HwSlotDraft::Perf have = HwSlotDraft::basePerf(&base);
    s.hasWinlock = want.winlock != have.winlock;
    s.winlock = want.winlock;
    s.hasIndicators = want.indicators != have.indicators;
    s.indicators = want.indicators;

    s.keepLight = HwLighting::same(draft.lighting, base.lighting);
    if(!s.keepLight){
        if(!draft.lighting.data().known){ error = "the lighting model is unknown"; return false; }
        s.lighting = draft.lighting;
    }

    // The bindings: the draft's when they are not the slot's. A slot that cannot be rewritten has none known: it keeps its own (the
    // check refuses it, and says why)
    if(base.state == HwBinding::Record::READONLY)
        s.keepBindings = true;
    else {
        if(!draft.hasBindings){ error = "the slot's bindings were not read"; return false; }
        s.keepBindings = HwBinding::sameBindings(draft.keys, base.keys);
    }
    if(!s.keepBindings)
        s.keys = draft.keys;

    const std::vector<std::string> words = HwBinding::stageWords(s);
    if(words.empty()){ error = "the slot cannot be said in the protocol"; return false; }
    // The lines as they go (with their newline, within the length the protocol allows)
    for(const std::string& line : HwBinding::packLines(owner, words)){
        if(line.size() + 1 > HwBinding::LINE_BYTES){ error = "a line of the preparation is too long"; return false; }
        result.prepare.push_back(line + "\n");
    }
    result.check = HwBinding::checkLine(owner, s) + "\n";
    result.save = HwBinding::saveLine(owner, s) + "\n";
    result.abort = HwBinding::abortLine(owner, s) + "\n";
    out = std::move(result);
    return true;
}
