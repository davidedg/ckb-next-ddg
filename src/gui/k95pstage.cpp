#include "k95pstage.h"

#include "k95pledmap.h"
#include "k95psnapshot.h"

#include <array>
#include <cstdio>

namespace {
int ledForName(const QString& name){
    for(size_t i = 0; i < K95PLedMap::count; ++i)
        if(name == QLatin1String(K95PLedMap::entries[i].name))
            return K95PLedMap::entries[i].led;
    return -1;
}
}

bool makeK95PStage(const KeyMap& map, const QColorMap& colours, const QString& name, const QString& baseName,
                   bool lightsChanged, const HwBinding::Record& base, const HwSlotDraft::Draft& draft,
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

    // The performance settings: wl: and ind: when the draft's are not the slot's
    const HwSlotDraft::Perf want = HwSlotDraft::effectivePerf(draft, &base);
    const HwSlotDraft::Perf have = HwSlotDraft::basePerf(&base);
    s.hasWinlock = want.winlock != have.winlock;
    s.winlock = want.winlock;
    s.hasIndicators = want.indicators != have.indicators;
    s.indicators = want.indicators;

    // The lighting: static layers of the mode's colours when they changed, or when they replace the slot's effects
    const bool image = base.light == HwBinding::Record::STATIC || base.light == HwBinding::Record::NONE;
    result.pic = image ? lightsChanged : draft.replaceLights;
    s.keepLight = !result.pic;
    if(result.pic){
        K95PSnapshot snap;
        if(!makeK95PSnapshot(map, colours, name, mode, snap, error))
            return false;
        // One name per LED (two names may be one LED): the one whose colour the picture has, as makeK95PSnapshot takes it
        std::array<QString, 256> ledName;
        QStringList names = map.keys();
        names.sort(Qt::CaseSensitive);
        for(const QString& keyName : names){
            const int led = ledForName(keyName);
            if(map.key(keyName).hasLed && led >= 0 && led < int(ledName.size()))
                ledName[size_t(led)] = keyName;
        }
        // The three indicator buttons are not the mode's to paint: they have the colours of the performance settings (the Win Lock
        // button at rest, its lock off colour), by LED, whatever name the keymap gives it (logo and profswitch are one LED)
        const struct { size_t led; size_t at; } buttons[] = { { 0x7d, 0 }, { 0x89, 3 }, { 0x08, 9 } };
        for(const auto& b : buttons)
            snap.ledRgb[b.led] = uint32_t(want.indicators[b.at]) << 16 | uint32_t(want.indicators[b.at + 1]) << 8 | want.indicators[b.at + 2];
        for(size_t led = 0; led < snap.ledRgb.size(); ++led)
            if(snap.ledRgb[led] && !ledName[led].isEmpty())
                s.rgb[ledName[led].toStdString()] = snap.ledRgb[led];
        result.colours = snap.colours;
        result.invisibleKeys = snap.invisibleKeys;
        result.rgbRequired = !snap.allBlack;
    } else
        result.rgbRequired = base.light == HwBinding::Record::STATIC && base.layers > 0;

    // The bindings: the draft's when they are not the slot's; a slot whose bindings are not a model keeps them unless recreated
    if(base.state == HwBinding::Record::RAW || base.state == HwBinding::Record::BROKEN){
        s.keepBindings = !draft.recreate;
        s.recreate = draft.recreate;
    } else {
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
