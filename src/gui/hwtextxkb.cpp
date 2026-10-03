#include "hwtextxkb.h"

#include "k95pkeytable.h"

#include <map>
#include <set>
#include <xkbcommon/xkbcommon.h>

using HwTextShape::Stroke;

namespace {

const xkb_keycode_t EVDEV_OFFSET = 8;           // an XKB keycode is the Linux input code + 8
const int KEY_LEFTSHIFT_CODE = 42, KEY_RIGHTALT_CODE = 100;

bool keypad(int code){
    return code == 55 || (code >= 71 && code <= 83) || code == 96 || code == 98 || code == 117 || code == 121;
}

// The character a key makes with left Shift and/or right Alt held, as the keymap says (0 if none, or a control character)
char32_t make(xkb_keymap* km, xkb_keycode_t kc, bool shift, bool altgr){
    xkb_state* st = xkb_state_new(km);
    if(!st)
        return 0;
    if(altgr) xkb_state_update_key(st, KEY_RIGHTALT_CODE + EVDEV_OFFSET, XKB_KEY_DOWN);
    if(shift) xkb_state_update_key(st, KEY_LEFTSHIFT_CODE + EVDEV_OFFSET, XKB_KEY_DOWN);
    char32_t c = xkb_state_key_get_utf32(st, kc);
    xkb_state_unref(st);
    if(c == '\r' || c == '\t')
        return c;
    return c < 0x20 || c == 0x7f ? 0 : c;
}

} // namespace

struct HwTextXkb::Impl {
    xkb_context* ctx = nullptr;
    xkb_keymap* km = nullptr;
    bool iso = true;
    bool filtered = false;                  // right Alt only on the allowed keys
    std::set<std::pair<int, bool>> allowed;
    std::set<int> skipped;                  // keys never pressed
    std::map<char32_t, Stroke> table;       // the first key found for each character
    std::set<char32_t> offWindows;          // made only by keys left out of allowed

    ~Impl(){
        if(km) xkb_keymap_unref(km);
        if(ctx) xkb_context_unref(ctx);
    }

    void build(){
        const xkb_keycode_t lo = xkb_keymap_min_keycode(km), hi = xkb_keymap_max_keycode(km);
        for(int pass = 0; pass < 2; ++pass)                 // the main block, then the keypad
            for(int form = 0; form < 4; ++form){            // alone, Shift, AltGr, AltGr and Shift
                const bool shift = form == 1 || form == 3, altgr = form >= 2;
                for(xkb_keycode_t kc = lo; kc <= hi && kc >= EVDEV_OFFSET; ++kc){
                    const int code = static_cast<int>(kc - EVDEV_OFFSET);
                    if(keypad(code) != (pass == 1) || code == KEY_LEFTSHIFT_CODE || code == KEY_RIGHTALT_CODE)
                        continue;
                    const int index = K95PKeyTable::indexOfEvdev(code, iso);
                    if(index < 0 || index >= static_cast<int>(K95PKeyTable::KEYS))
                        continue;
                    const char32_t c = make(km, kc, shift, altgr);
                    if(!c || table.count(c))
                        continue;
                    if(skipped.count(code) || (altgr && filtered && !allowed.count(std::make_pair(code, shift)))){
                        offWindows.insert(c);
                        continue;
                    }
                    Stroke s;
                    s.key = static_cast<uint8_t>(index);
                    s.shift = shift;
                    s.altgr = altgr;
                    table[c] = s;
                }
            }
        for(const auto& kv : table)
            offWindows.erase(kv.first);
    }

    void allow(const std::vector<std::pair<int, bool>>* keys, const std::vector<int>* skip){
        filtered = keys != nullptr;
        if(keys) allowed.insert(keys->begin(), keys->end());
        if(skip) skipped.insert(skip->begin(), skip->end());
    }
};

HwTextXkb::HwTextXkb(Impl* impl) : impl_(impl){}
HwTextXkb::~HwTextXkb() = default;

std::unique_ptr<HwTextXkb> HwTextXkb::fromNames(const std::string& layout, const std::string& variant, bool iso,
                                                const std::vector<std::pair<int, bool>>* altgr,
                                                const std::vector<int>* skip){
    std::unique_ptr<Impl> impl(new Impl);
    impl->iso = iso;
    impl->allow(altgr, skip);
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if(!impl->ctx)
        return nullptr;
    xkb_rule_names names = { "evdev", "pc105", layout.c_str(), variant.c_str(), "" };
    impl->km = xkb_keymap_new_from_names(impl->ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if(!impl->km)
        return nullptr;
    impl->build();
    return std::unique_ptr<HwTextXkb>(new HwTextXkb(impl.release()));
}

std::unique_ptr<HwTextXkb> HwTextXkb::fromString(const std::string& keymap, bool iso,
                                                 const std::vector<std::pair<int, bool>>* altgr,
                                                 const std::vector<int>* skip){
    std::unique_ptr<Impl> impl(new Impl);
    impl->iso = iso;
    impl->allow(altgr, skip);
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if(!impl->ctx)
        return nullptr;
    impl->km = xkb_keymap_new_from_string(impl->ctx, keymap.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if(!impl->km)
        return nullptr;
    impl->build();
    return std::unique_ptr<HwTextXkb>(new HwTextXkb(impl.release()));
}

std::vector<Stroke> HwTextXkb::strokes(const std::u32string& text, std::u32string* rejected, std::u32string* notOnWindows) const {
    std::vector<Stroke> out;
    if(rejected) rejected->clear();
    if(notOnWindows) notOnWindows->clear();
    for(size_t i = 0; i < text.size(); ++i){
        char32_t c = text[i];
        if(c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;   // "\r\n" is one Enter
        if(c == '\n') c = '\r';
        const auto it = impl_->table.find(c);
        if(it != impl_->table.end()){
            out.push_back(it->second);
            continue;
        }
        std::u32string* to = notOnWindows && impl_->offWindows.count(c) ? notOnWindows : rejected;
        if(to && to->find(text[i]) == std::u32string::npos) to->push_back(text[i]);
    }
    return out;
}

char32_t HwTextXkb::character(const Stroke& s) const {
    const int code = K95PKeyTable::evdev(s.key);
    if(code < 0 || K95PKeyTable::indexOfEvdev(code, impl_->iso) != s.key)
        return 0;
    const char32_t c = make(impl_->km, static_cast<xkb_keycode_t>(code) + EVDEV_OFFSET, s.shift, s.altgr);
    return c == '\r' ? U'\n' : c;
}
