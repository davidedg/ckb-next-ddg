#include "hwbindedit.h"
#include "hwtextshape.h"
#include "k95pkeytable.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace HwBindEdit {

using HwBinding::Action;
using HwBinding::Event;

namespace {

using HwBinding::press;
using HwBinding::release;
using HwBinding::delay;   // in iCUE's form: 2 bytes below 8192 ms, 4 from there

std::string hex2(unsigned v){
    static const char d[] = "0123456789abcdef";
    return std::string(1, d[(v >> 4) & 15]) + d[v & 15];
}

} // namespace

bool icueValues(uint8_t subtype, uint8_t start, uint8_t run, uint8_t repeat){
    if(subtype == HwBinding::SHORTCUT && start == START_PRESS && run == RUN_IMITATE_TOGGLE && repeat == 0)
        return true;   // 'Imitate holding key' Toggle
    if(repeat != 1)
        return false;
    if(subtype == HwBinding::MACRO)
        return (start == START_PRESS && (run == RUN_ONCE || run == RUN_WHILE_PRESSED || run == RUN_TOGGLE))
               || (start == START_RELEASE && run == RUN_ONCE);   // iCUE's four alternatives
    if(subtype == HwBinding::SHORTCUT || subtype == HwBinding::TEXT)
        return start == START_PRESS && run == RUN_ONCE;
    return false;
}

namespace {
// A delay in the form iCUE writes: 2 bytes below 8192 ms, 4 from there
bool icueDelay(const Event& e){
    const HwBinding::EventKind k = HwBinding::eventKind(e);
    return k == HwBinding::EV_DELAY || (k == HwBinding::EV_LONG && HwBinding::delayMs(e) > HwBinding::DELAY_SHORT_MAX);
}
bool isKey(const Event& e){
    const HwBinding::EventKind k = HwBinding::eventKind(e);
    return k == HwBinding::EV_PRESS || k == HwBinding::EV_RELEASE;
}
// 'Imitate holding key': a Keystroke (subtype 01) of a key pressed, a delay and the key released, run 84 (Toggle, a delay of 0) or a
// delay that is not 0 (On press). The other Keystrokes are iCUE's Keystroke
bool imitateShape(const Action& a){
    if(a.kind != Action::MACRO || a.subtype != HwBinding::SHORTCUT) return false;
    if(a.run == RUN_IMITATE_TOGGLE) return true;
    return a.events.size() == 3 && !(HwBinding::eventKind(a.events[1]) == HwBinding::EV_DELAY && HwBinding::delayMs(a.events[1]) == 0);
}
// iCUE's clamp of a time of 'Imitate' On press above 16 777.2 s: shown Kept, but iCUE writes it (check())
bool imitateClamp(const Event& e){
    return e.b0 == 0xc0 && e.b1 == 0xff && e.b2 == 0xff && e.b3 == 0xff;
}
// The key a Keystroke presses besides Win, Ctrl, Alt and Shift is a key of iCUE's keyboard (not a media key)
bool keystrokeKeysOffered(const std::vector<uint8_t>& keys);
} // namespace

bool recognizeImitate(const Action& a, uint8_t& key, unsigned& ms, bool& toggle){
    key = 0; ms = 0; toggle = false;
    if(!imitateShape(a) || !icueValues(a.subtype, a.start, a.run, a.repeat) || a.events.size() != 3) return false;
    const Event& p = a.events[0];
    const Event& d = a.events[1];
    const Event& r = a.events[2];
    if(HwBinding::eventKind(p) != HwBinding::EV_PRESS || r != HwBinding::release(p.b1) || !K95PKeyTable::icueKeyboard(p.b1)) return false;
    toggle = a.run == RUN_IMITATE_TOGGLE;
    if(toggle){
        if(HwBinding::eventKind(d) != HwBinding::EV_DELAY || HwBinding::delayMs(d) != 0) return false;
    } else {
        if(!icueDelay(d)) return false;
        ms = HwBinding::delayMs(d);
        if(ms < IMITATE_MIN_MS || ms > IMITATE_MAX_MS || ms % 100) return false;
    }
    key = p.b1;
    return true;
}

Action imitatePress(uint8_t key, unsigned ms){
    Action a;
    a.kind = Action::MACRO;
    a.subtype = HwBinding::SHORTCUT;
    a.start = START_PRESS;
    a.run = RUN_ONCE;
    a.repeat = 1;
    a.events = { press(key), delay(ms), release(key) };
    return a;
}

Action imitateToggle(uint8_t key){
    Action a = imitatePress(key, 0);
    a.run = RUN_IMITATE_TOGGLE;
    a.repeat = 0;
    return a;
}

Kind kindOf(const Action& a){
    if(a.kind == Action::NATIVE) return NATIVE;
    if(a.kind == Action::REMAP){
        if(a.dst >= K95PKeyTable::MOUSE_FIRST && a.dst <= K95PKeyTable::MOUSE_LAST) return REMAP_MOUSE;
        if(K95PKeyTable::icueKeyboard(a.dst) || K95PKeyTable::icueMedia(a.dst)) return REMAP_KEYBOARD;
        if(K95PKeyTable::icueLanguage(a.dst)) return REMAP_LANGUAGE;
        return KEPT;
    }
    if(!icueValues(a.subtype, a.start, a.run, a.repeat)) return KEPT;
    if(a.subtype == HwBinding::SHORTCUT){
        uint8_t key; unsigned ms; bool toggle;
        if(imitateShape(a)) return recognizeImitate(a, key, ms, toggle) ? (toggle ? IMITATE_TOGGLE : IMITATE_PRESS) : KEPT;
        if(a.run != RUN_ONCE) return KEPT;
        std::vector<uint8_t> keys;
        return recognizeShortcut(a.events, keys) && isIcueShortcut(keys) && keystrokeKeysOffered(keys) ? SHORTCUT : KEPT;
    }
    if(a.subtype == HwBinding::MACRO){
        // every delay of 2 bytes and at most 4095 ms, a random one from at most to: as iCUE's editor makes them
        std::vector<Row> r;
        if(!rows(a.events, r)) return KEPT;
        for(size_t i = 0; i < a.events.size(); ++i){
            const HwBinding::EventKind k = HwBinding::eventKind(a.events[i]);
            if(k == HwBinding::EV_LONG || (k == HwBinding::EV_DELAY && r[i].value > MACRO_DELAY_MAX)) return KEPT;
            if(k == HwBinding::EV_RANDOM && r[i].value > r[i].max) return KEPT;
        }
        return MACRO;
    }
    if(a.subtype == HwBinding::TEXT){
        std::vector<HwTextShape::Stroke> strokes;
        unsigned ms = 0;
        return HwTextShape::recognize(a.events, strokes, ms) ? TEXT : KEPT;
    }
    return KEPT;
}

std::vector<Event> shortcutEvents(const std::vector<uint8_t>& keys){
    std::vector<Event> out;
    for(size_t i = 0; i < keys.size(); ++i){
        if(!out.empty()) out.push_back(delay(0));
        out.push_back(press(keys[i]));
    }
    for(size_t i = keys.size(); i-- > 0;){
        out.push_back(delay(0));
        out.push_back(release(keys[i]));
    }
    return out;
}

bool recognizeShortcut(const std::vector<Event>& ev, std::vector<uint8_t>& keys){
    keys.clear();
    if(ev.empty() || ev.size() % 4 != 3) return false;
    const size_t n = (ev.size() + 1) / 4;
    for(size_t i = 0; i < n; ++i){
        const Event& p = ev[2 * i];
        if(p.b0 != 0x20 || p.b1 >= HwBinding::KEYS) return false;
        for(uint8_t k : keys) if(k == p.b1) return false;
        keys.push_back(p.b1);
    }
    if(shortcutEvents(keys) != ev){ keys.clear(); return false; }
    return true;
}

namespace {
// The modifiers of iCUE's Keystroke, in the order of its presses, and the right ones that become them (key indices of the files)
struct Modifier { const char* left; const char* right; };
const Modifier MODIFIERS[] = { {"lwin", "rwin"}, {"lctrl", "rctrl"}, {"lalt", "ralt"}, {"lshift", "rshift"} };
constexpr unsigned SHORTCUT_MODIFIERS_MAX = 3;

int modifierOf(uint8_t k){
    for(unsigned i = 0; i < sizeof(MODIFIERS) / sizeof(MODIFIERS[0]); ++i)
        if(K95PKeyTable::indexOf(MODIFIERS[i].left) == int(k) || K95PKeyTable::indexOf(MODIFIERS[i].right) == int(k))
            return int(i);
    return -1;
}
} // namespace

bool shortcutKeys(std::vector<uint8_t>& keys, std::string& why){
    why.clear();
    bool mod[sizeof(MODIFIERS) / sizeof(MODIFIERS[0])] = {};
    std::vector<uint8_t> others;
    unsigned mods = 0;
    for(uint8_t k : keys){
        const int m = modifierOf(k);
        if(m >= 0){
            if(!mod[m]) ++mods;
            mod[m] = true;
        } else if(std::find(others.begin(), others.end(), k) == others.end())
            others.push_back(k);
    }
    if(mods + others.size() == 0){ why = "a shortcut needs at least one key"; return false; }
    if(mods > SHORTCUT_MODIFIERS_MAX){ why = "a shortcut has at most three of Win, Ctrl, Alt and Shift"; return false; }
    if(others.size() > 1){ why = "a shortcut has at most one key besides Win, Ctrl, Alt and Shift"; return false; }
    keys.clear();
    for(unsigned i = 0; i < sizeof(MODIFIERS) / sizeof(MODIFIERS[0]); ++i)
        if(mod[i]) keys.push_back(uint8_t(K95PKeyTable::indexOf(MODIFIERS[i].left)));
    keys.insert(keys.end(), others.begin(), others.end());
    return true;
}

namespace {
bool keystrokeKeysOffered(const std::vector<uint8_t>& keys){
    for(uint8_t k : keys)
        if(modifierOf(k) < 0 && !K95PKeyTable::icueKeyboard(k)) return false;
    return true;
}
} // namespace

bool isIcueShortcut(const std::vector<uint8_t>& keys){
    // Made again, they are the same keys only if they already kept the rules, in iCUE's order, with no right modifier
    std::vector<uint8_t> made = keys;
    std::string why;
    return shortcutKeys(made, why) && made == keys;
}

bool rows(const std::vector<Event>& ev, std::vector<Row>& out){
    out.clear();
    for(const Event& e : ev){
        Row r;
        // a delay of 2 or 4 bytes is a row of its value, a random one a row of its bounds; an odd or unknown event is none
        const HwBinding::EventKind kind = HwBinding::eventKind(e);
        if(kind == HwBinding::EV_DELAY || kind == HwBinding::EV_LONG){ r.type = Row::DELAY; r.value = HwBinding::delayMs(e); }
        else if(kind == HwBinding::EV_RANDOM){ r.type = Row::RANDOM; r.value = HwBinding::randomMin(e); r.max = HwBinding::randomMax(e); }
        else if(kind == HwBinding::EV_PRESS){ r.type = Row::PRESS; r.value = e.b1; }
        else if(kind == HwBinding::EV_RELEASE){ r.type = Row::RELEASE; r.value = e.b1; }
        else { out.clear(); return false; }
        if((r.type == Row::PRESS || r.type == Row::RELEASE) && r.value >= HwBinding::KEYS){ out.clear(); return false; }
        out.push_back(r);
    }
    return true;
}

size_t moveRows(std::vector<Row>& rowsIn, size_t first, size_t count, size_t to){
    const size_t size = rowsIn.size();
    if(count == 0 || first >= size || count > size - first || to > size || (to >= first && to <= first + count))
        return first;
    const std::vector<Row> block(rowsIn.begin() + long(first), rowsIn.begin() + long(first + count));
    rowsIn.erase(rowsIn.begin() + long(first), rowsIn.begin() + long(first + count));
    const size_t at = to > first ? to - count : to;
    rowsIn.insert(rowsIn.begin() + long(at), block.begin(), block.end());
    return at;
}

size_t insertRows(std::vector<Row>& rowsIn, size_t at, const std::vector<Row>& copies){
    if(at > rowsIn.size()) at = rowsIn.size();
    rowsIn.insert(rowsIn.begin() + long(at), copies.begin(), copies.end());
    return at;
}

bool events(const std::vector<Row>& rowsIn, std::vector<Event>& out, std::string& why){
    out.clear();
    why.clear();
    if(rowsIn.empty()){
        why = "a macro has at least one row";
        return false;
    }
    bool down[HwBinding::KEYS] = {};
    std::vector<Event> ev;
    bool lastKey = false;
    for(size_t i = 0; i < rowsIn.size(); ++i){
        const Row& r = rowsIn[i];
        const std::string at = "row " + std::to_string(i + 1) + ": ";
        if(r.type == Row::DELAY){
            if(r.value > MACRO_DELAY_MAX){ why = at + "a delay of a macro is at most " + std::to_string(MACRO_DELAY_MAX) + " ms"; return false; }
            ev.push_back(delay(r.value));
            lastKey = false;
            continue;
        }
        if(r.type == Row::RANDOM){
            if(r.max > MACRO_DELAY_MAX){ why = at + "a delay of a macro is at most " + std::to_string(MACRO_DELAY_MAX) + " ms"; return false; }
            if(r.value > r.max){ why = at + "a random delay goes from the shorter time to the longer one"; return false; }
            ev.push_back(HwBinding::randomDelay(r.value, r.max));
            lastKey = false;
            continue;
        }
        if(r.value >= HwBinding::KEYS || K95PKeyTable::label(r.value).empty()){ why = at + "not a key"; return false; }
        const std::string name = K95PKeyTable::label(r.value);
        // iCUE's writer puts a delay of 0 between two key events
        if(lastKey) ev.push_back(delay(0));
        if(r.type == Row::PRESS){
            if(down[r.value]){ why = at + name + " is pressed again before it is released"; return false; }
            down[r.value] = true;
            ev.push_back(press(uint8_t(r.value)));
        } else {
            if(!down[r.value]){ why = at + name + " is released but not pressed"; return false; }
            down[r.value] = false;
            ev.push_back(release(uint8_t(r.value)));
        }
        lastKey = true;
    }
    for(unsigned k = 0; k < HwBinding::KEYS; ++k)
        if(down[k]){ why = K95PKeyTable::label(k) + " is pressed and never released"; return false; }
    if(ev.size() > MACRO_ROWS_MAX){
        why = "a macro has at most " + std::to_string(MACRO_ROWS_MAX) + " rows, and this one would have " + std::to_string(ev.size()) +
              " (iCUE puts a delay of 0 between two key rows next to each other)";
        return false;
    }
    out = ev;
    return true;
}

unsigned savedRows(const std::vector<Event>& ev){
    unsigned n = 0;
    for(size_t i = 0; i < ev.size(); ++i){
        const bool zeroBetween = i > 0 && i + 1 < ev.size() && HwBinding::eventKind(ev[i]) == HwBinding::EV_DELAY &&
                                 HwBinding::delayMs(ev[i]) == 0 && isKey(ev[i - 1]) && isKey(ev[i + 1]);
        n += !zeroBetween;
    }
    return n;
}

Action native(){ return Action(); }

Action remap(uint8_t dst){
    Action a;
    a.kind = Action::REMAP;
    a.dst = dst;
    return a;
}

static Action macroOf(uint8_t subtype, const std::vector<Event>& ev, uint8_t start, uint8_t run){
    Action a;
    a.kind = Action::MACRO;
    a.subtype = subtype;
    a.start = start;
    a.run = run;
    a.repeat = 1;
    a.events = ev;
    return a;
}

Action macro(const std::vector<Event>& ev, uint8_t start, uint8_t run){ return macroOf(HwBinding::MACRO, ev, start, run); }
Action shortcut(const std::vector<uint8_t>& keys){
    std::vector<uint8_t> made = keys;
    std::string why;
    if(!shortcutKeys(made, why)) made = keys;   // check() says why
    return macroOf(HwBinding::SHORTCUT, shortcutEvents(made), START_PRESS, RUN_ONCE);
}
Action text(const std::vector<Event>& ev){ return macroOf(HwBinding::TEXT, ev, START_PRESS, RUN_ONCE); }

namespace {
// The checks of one macro action, by its kind (the daemon's cape_binding_check, plus the shape of a Text's characters)
bool checkMacro(const Action& a, std::string& why){
    if(!icueValues(a.subtype, a.start, a.run, a.repeat)){ why = "not an action iCUE writes"; return false; }
    if(a.events.empty() || a.events.size() > EVENTS_MAX){ why = "an action has 1 to " + std::to_string(EVENTS_MAX) + " events"; return false; }
    bool down[HwBinding::KEYS] = {};
    for(const Event& e : a.events){
        const HwBinding::EventKind k = HwBinding::eventKind(e);
        if(k == HwBinding::EV_UNKNOWN || k == HwBinding::EV_ODD_SHORT){ why = "an event that iCUE does not write"; return false; }
        if(k != HwBinding::EV_PRESS && k != HwBinding::EV_RELEASE) continue;
        if(e.b1 >= HwBinding::KEYS){ why = "an event of a key outside the files"; return false; }
        const std::string name = K95PKeyTable::label(e.b1);
        if(k == HwBinding::EV_PRESS){
            if(down[e.b1]){ why = name + " is pressed again before it is released"; return false; }
            down[e.b1] = true;
        } else {
            if(!down[e.b1]){ why = name + " is released but not pressed"; return false; }
            down[e.b1] = false;
        }
    }
    for(unsigned k = 0; k < HwBinding::KEYS; ++k)
        if(down[k]){ why = K95PKeyTable::label(k) + " is pressed and never released"; return false; }
    if(a.subtype == HwBinding::MACRO){
        for(const Event& e : a.events){
            const HwBinding::EventKind k = HwBinding::eventKind(e);
            if(k == HwBinding::EV_LONG || (k == HwBinding::EV_DELAY && HwBinding::delayMs(e) > MACRO_DELAY_MAX)){
                why = "a delay of a macro is at most " + std::to_string(MACRO_DELAY_MAX) + " ms";
                return false;
            }
            if(k == HwBinding::EV_RANDOM && HwBinding::randomMin(e) > HwBinding::randomMax(e)){
                why = "a random delay goes from the shorter time to the longer one";
                return false;
            }
        }
        // what iCUE can have written: the delays of 0 its writer puts between two key events are no rows of its editor
        if(savedRows(a.events) > MACRO_ROWS_MAX){
            why = "a macro has at most " + std::to_string(MACRO_ROWS_MAX) + " rows";
            return false;
        }
        return true;
    }
    if(a.subtype == HwBinding::SHORTCUT){
        if(imitateShape(a)){
            uint8_t key; unsigned ms; bool toggle;
            // iCUE's clamp of longer times is iCUE's too (a copy of one of its slots brings it), though the editor never makes it
            const bool clamp = a.run == RUN_ONCE && a.events.size() == 3 && imitateClamp(a.events[1]) &&
                               HwBinding::eventKind(a.events[0]) == HwBinding::EV_PRESS && K95PKeyTable::icueKeyboard(a.events[0].b1);
            if(!clamp && !recognizeImitate(a, key, ms, toggle)){
                why = "not 'Imitate holding key' as iCUE writes it (a key of its keyboard, held 0.1 to 16777.2 s in steps of 0.1 s, "
                      "or toggled with a delay of 0)";
                return false;
            }
            return true;
        }
        std::vector<uint8_t> keys;
        if(a.run != RUN_ONCE || !recognizeShortcut(a.events, keys) || !isIcueShortcut(keys)){
            std::string rule;
            if(!keys.empty() && !shortcutKeys(keys, rule)) why = rule;
            else why = "not a shortcut iCUE writes (up to three of Win, Ctrl, Alt, Shift in this order, then one key)";
            return false;
        }
        if(!keystrokeKeysOffered(keys)){ why = "a shortcut has a key of iCUE's keyboard (no media key)"; return false; }
        return true;
    }
    if(a.subtype == HwBinding::TEXT){
        std::vector<HwTextShape::Stroke> strokes;
        unsigned ms = 0;
        if(!HwTextShape::recognize(a.events, strokes, ms)){
            why = "not a Text as iCUE writes it (the keys of its characters, delays of 0 inside them, up to " +
                  std::to_string(TEXT_DELAY_MAX) + " ms between them)";
            return false;
        }
        return true;
    }
    why = "not an action iCUE writes";
    return false;
}
} // namespace

bool check(const HwBinding::Keys& draft, const HwBinding::Keys* base, std::string& why){
    why.clear();
    // No count of the macros: a draft holds one action per key, and a slot holds a macro file for every key (the daemon's
    // CAPE_SLOT_MACROS_MAX, 152). No count of the bytes either: the daemon checks the free space of the flash
    for(unsigned k = 0; k < HwBinding::KEYS; ++k){
        const Action& a = draft[k];
        if(base && a.sameAs((*base)[k]))
            continue;
        const std::string key = K95PKeyTable::label(k).empty() ? "#" + hex2(k) : K95PKeyTable::label(k);
        // (a destination that is neither a key nor a mouse button is none of iCUE's either)
        if(a.kind == Action::REMAP && !K95PKeyTable::isIcueDestination(a.dst)){
            const std::string dst = K95PKeyTable::label(a.dst);
            why = key + ": iCUE does not offer " + (dst.empty() ? "#" + hex2(a.dst) : dst) + " as a destination";
            return false;
        }
        std::string err;
        if(a.kind == Action::MACRO && !checkMacro(a, err)){
            why = key + ": " + err;
            return false;
        }
    }
    return true;
}

bool xkbNames(const std::string& ckb, std::string& layout, std::string& variant){
    static const std::map<std::string, std::pair<const char*, const char*>> table = {
        {"dk", {"dk", ""}}, {"eu", {"us", ""}}, {"eu_dvorak", {"us", "dvorak"}}, {"gb", {"gb", ""}},
        {"gb_dvorak", {"gb", "dvorak"}}, {"us", {"us", ""}}, {"us_dvorak", {"us", "dvorak"}}, {"fr", {"fr", ""}},
        {"de", {"de", ""}}, {"it", {"it", ""}}, {"jp", {"jp", ""}}, {"no", {"no", ""}}, {"pl", {"pl", ""}},
        {"pt_br", {"br", ""}}, {"mx", {"latam", ""}}, {"es", {"es", ""}}, {"se", {"se", ""}},
    };
    const auto it = table.find(ckb);
    if(it == table.end()) return false;
    layout = it->second.first;
    variant = it->second.second;
    return true;
}

bool windowsAltGr(const std::string& ckb, std::vector<std::pair<int, bool>>& keys, std::vector<int>* skip){
    keys.clear();
    if(skip) skip->clear();
    const bool english = ckb == "eu" || ckb == "eu_dvorak" || ckb == "us" || ckb == "us_dvorak";
    // KEY_102ND: "\|" on Windows US (and in a BIOS), "<>" in XKB's "us" on an ISO keyboard
    if(english && skip) *skip = {86};
    // Italian (KBDIT): AltGr+è [, AltGr++ ], AltGr+ò @, AltGr+à #, AltGr+E €; AltGr+Shift+è {, AltGr+Shift++ }
    // (KEY_LEFTBRACE 26, KEY_RIGHTBRACE 27, KEY_SEMICOLON 39, KEY_APOSTROPHE 40, KEY_E 18)
    if(ckb == "it"){
        keys = {{26, false}, {27, false}, {39, false}, {40, false}, {18, false}, {26, true}, {27, true}};
        return true;
    }
    // The English layouts have no AltGr: nothing to allow, and nothing is lost
    return english;
}

} // namespace HwBindEdit
