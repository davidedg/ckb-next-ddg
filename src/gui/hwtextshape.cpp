#include "hwtextshape.h"

#include "k95pkeytable.h"

namespace HwTextShape {

using HwBinding::Event;

namespace {

using HwBinding::press;
using HwBinding::release;
using HwBinding::delay;   // in iCUE's form: 2 bytes below 8192 ms, 4 from there

// The key events of one character
void keys_of(const Stroke& s, std::vector<Event>& out){
    if(s.altgr) out.push_back(press(RALT));
    if(s.shift) out.push_back(press(LSHIFT));
    out.push_back(press(s.key));
    out.push_back(release(s.key));
    if(s.shift) out.push_back(release(LSHIFT));
    if(s.altgr) out.push_back(release(RALT));
}

uint8_t key(const char* name){
    return static_cast<uint8_t>(K95PKeyTable::indexOf(name));
}

} // namespace

std::vector<Event> events(const std::vector<Stroke>& strokes, unsigned delayMs){
    std::vector<Event> out;
    if(delayMs > DELAY_MAX) return out;
    for(size_t i = 0; i < strokes.size(); ++i){
        std::vector<Event> k;
        keys_of(strokes[i], k);
        for(size_t j = 0; j < k.size(); ++j){
            if(!out.empty())
                out.push_back(delay(j == 0 ? delayMs : 0));
            out.push_back(k[j]);
        }
    }
    return out;
}

bool recognize(const std::vector<Event>& ev, std::vector<Stroke>& strokes, unsigned& delayMs){
    strokes.clear();
    delayMs = 0;
    // key events at even positions, delays at odd ones
    if(ev.empty() || ev.size() % 2 == 0) return false;
    std::vector<Event> keys;
    std::vector<unsigned> gaps;
    for(size_t i = 0; i < ev.size(); ++i){
        // a delay only in iCUE's form (a long one from 8192 ms); a random or odd one is no delay here, so its place refuses it
        const HwBinding::EventKind kind = HwBinding::eventKind(ev[i]);
        if(kind == HwBinding::EV_LONG && HwBinding::delayMs(ev[i]) <= HwBinding::DELAY_SHORT_MAX) return false;
        const bool isDelay = kind == HwBinding::EV_DELAY || kind == HwBinding::EV_LONG;
        if(isDelay != (i % 2 == 1)) return false;
        if(isDelay && HwBinding::delayMs(ev[i]) > DELAY_MAX) return false;
        if(isDelay) gaps.push_back(HwBinding::delayMs(ev[i]));
        else keys.push_back(ev[i]);
    }
    bool haveDelay = false;
    size_t k = 0;
    while(k < keys.size()){
        // the longest shape that matches here: AltGr and Shift, AltGr, Shift, plain
        Stroke s;
        size_t n = 0;
        for(int form = 0; form < 4 && !n; ++form){
            Stroke t;
            t.altgr = form == 0 || form == 1;
            t.shift = form == 0 || form == 2;
            const size_t need = 2 + (t.altgr ? 2 : 0) + (t.shift ? 2 : 0);
            if(k + need > keys.size()) continue;
            const size_t mid = k + (t.altgr ? 1 : 0) + (t.shift ? 1 : 0);
            t.key = keys[mid].b1;
            if(t.key == RALT || t.key == LSHIFT) continue;
            std::vector<Event> want;
            keys_of(t, want);
            bool same = true;
            for(size_t j = 0; j < need && same; ++j)
                same = keys[k + j] == want[j];
            if(same){
                s = t;
                n = need;
            }
        }
        if(!n || s.key >= HwBinding::KEYS) return false;
        // the delays inside a character are 0; the one before it is the delay between characters, the same for all
        for(size_t j = 0; j + 1 < n; ++j)
            if(gaps[k + j] != 0) return false;
        if(k > 0){
            const unsigned d = gaps[k - 1];
            if(haveDelay && d != delayMs) return false;
            delayMs = d;
            haveDelay = true;
        }
        strokes.push_back(s);
        k += n;
    }
    return true;
}

bool icueUsStroke(char32_t c, Stroke& out){
    static const char* const lower = "abcdefghijklmnopqrstuvwxyz";
    out = Stroke();
    if(c >= 'a' && c <= 'z'){ out.key = key(std::string(1, lower[c - 'a']).c_str()); return true; }
    if(c >= 'A' && c <= 'Z'){ out.key = key(std::string(1, lower[c - 'A']).c_str()); out.shift = true; return true; }
    if(c >= '0' && c <= '9'){ out.key = key(std::string(1, static_cast<char>(c)).c_str()); return true; }
    struct { char32_t c; const char* key; bool shift; } table[] = {
        { ' ', "space", false }, { '\t', "tab", false }, { '\n', "enter", false }, { '\r', "enter", false },
        { '!', "1", true }, { '@', "2", true }, { '#', "3", true }, { '$', "4", true }, { '%', "5", true }, { '^', "6", true },
        { '&', "7", true }, { '*', "8", true }, { '(', "9", true }, { ')', "0", true },
        { '-', "minus", false }, { '_', "minus", true }, { '=', "equal", false }, { '+', "equal", true },
        { '[', "lbrace", false }, { '{', "lbrace", true }, { ']', "rbrace", false }, { '}', "rbrace", true },
        { '\\', "bslash", false }, { '|', "bslash", true }, { ';', "colon", false }, { ':', "colon", true },
        { '\'', "quote", false }, { '"', "quote", true }, { '`', "grave", false }, { '~', "grave", true },
        { ',', "comma", false }, { '<', "comma", true }, { '.', "dot", false }, { '>', "dot", true },
        { '/', "slash", false }, { '?', "slash", true },
    };
    for(const auto& t : table)
        if(t.c == c){
            out.key = key(t.key);
            out.shift = t.shift;
            return true;
        }
    return false;
}

std::vector<Stroke> icueUsStrokes(const std::u32string& text, std::u32string* dropped){
    std::vector<Stroke> out;
    if(dropped) dropped->clear();
    for(size_t i = 0; i < text.size(); ++i){
        if(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;   // "\r\n" is one Enter
        Stroke s;
        if(icueUsStroke(text[i], s)) out.push_back(s);
        else if(dropped) dropped->push_back(text[i]);
    }
    return out;
}

bool utf32(const std::string& s, std::u32string& out){
    out.clear();
    if(!HwBinding::validUtf8(s)) return false;
    for(size_t i = 0; i < s.size();){
        const unsigned char b = static_cast<unsigned char>(s[i]);
        const unsigned need = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        char32_t c = need == 1 ? b : need == 2 ? (b & 0x1f) : need == 3 ? (b & 0x0f) : (b & 0x07);
        for(unsigned j = 1; j < need; ++j)
            c = (c << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3f);
        out.push_back(c);
        i += need;
    }
    return true;
}

} // namespace HwTextShape
