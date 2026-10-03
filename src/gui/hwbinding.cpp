#include "hwbinding.h"

#include <cstdio>

namespace HwBinding {

namespace {

bool unreserved(unsigned char c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

int upper_hex(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lower_hex(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Exactly n lowercase hex digits
bool hex_exact(const std::string& s, size_t n, uint32_t& v){
    if(s.size() != n || n > 8) return false;
    v = 0;
    for(char c : s){
        const int d = lower_hex(c);
        if(d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    return true;
}

// A decimal number without leading zeros, at most max
bool dec(const std::string& s, unsigned long max, unsigned long& v){
    if(s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
    unsigned long long x = 0;
    for(char c : s){
        if(c < '0' || c > '9') return false;
        x = x * 10 + static_cast<unsigned>(c - '0');
    }
    if(x > max) return false;
    v = static_cast<unsigned long>(x);
    return true;
}

std::vector<std::string> split(const std::string& s, char sep, size_t max){
    std::vector<std::string> out;
    size_t start = 0;
    for(;;){
        const size_t at = out.size() + 1 < max ? s.find(sep, start) : std::string::npos;
        if(at == std::string::npos){
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

bool starts(const std::string& s, const char* prefix){
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}:rev" as :hwid prints it, or "0"
bool valid_id(const std::string& id){
    if(id == "0") return true;
    if(id.size() < 40 || id.size() > 47 || id[0] != '{' || id[37] != '}' || id[38] != ':') return false;
    for(size_t i = 1; i < 37; ++i){
        if(i == 9 || i == 14 || i == 19 || i == 24){
            if(id[i] != '-') return false;
        } else if(upper_hex(id[i]) < 0) return false;
    }
    const std::string rev = id.substr(39);
    if(rev.size() > 1 && rev[0] == '0') return false;
    for(char c : rev) if(lower_hex(c) < 0) return false;
    return true;
}

bool valid_dst(uint32_t d){
    return d < KEYS || (d >= 0xc8 && d <= 0xcc);
}

bool name_words(const std::vector<std::string>& w, size_t& i, const char* prefix, std::string& name, std::string& err){
    // prefix:<i>/<n>:<text>, i = 1..n in order
    unsigned long total = 0;
    std::string bytes;
    for(unsigned long want = 1;; ++want){
        if(i >= w.size() || !starts(w[i], prefix)){ err = "the name is not all there"; return false; }
        const std::vector<std::string> f = split(w[i].substr(std::char_traits<char>::length(prefix)), ':', 2);
        const std::vector<std::string> in = f.size() == 2 ? split(f[0], '/', 2) : std::vector<std::string>();
        unsigned long idx, n;
        if(in.size() != 2 || !dec(in[0], NAME_WORDS, idx) || !dec(in[1], NAME_WORDS, n) || idx != want || (total && n != total)
           || f[1].size() > TEXT_MAX){
            err = "a malformed name word";
            return false;
        }
        total = n;
        std::string piece;
        if(!decode(f[1], piece)){ err = "a name that is not percent-encoded as it should be"; return false; }
        bytes += piece;
        ++i;
        if(idx == total) break;
    }
    size_t units = 0;
    if(!validUtf8(bytes, &units) || units > NAME_UNITS){ err = "a name that is not UTF-8, or too long"; return false; }
    name = bytes;
    return true;
}

void add_events(std::vector<std::string>& w, const std::string& prefix, unsigned key, const std::vector<Event>& ev){
    // As many whole events as fit in EVENT_BYTES, as in the file (cape_hwslot.h)
    for(size_t off = 0; off < ev.size();){
        std::string word = prefix;
        char head[32];
        std::snprintf(head, sizeof(head), ":%02x:%u:", key, static_cast<unsigned>(off));
        word += head;
        size_t bytes = 0, i = off;
        for(; i < ev.size() && bytes + ev[i].size() <= EVENT_BYTES; ++i){
            char hex[9];
            if(ev[i].size() == 4) std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", ev[i].b0, ev[i].b1, ev[i].b2, ev[i].b3);
            else std::snprintf(hex, sizeof(hex), "%02x%02x", ev[i].b0, ev[i].b1);
            word += hex;
            bytes += ev[i].size();
        }
        w.push_back(word);
        off = i;
    }
}

bool valid_led(const std::string& led){
    if(led.empty() || led.size() > 15) return false;
    for(char c : led)
        if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool event_known(const Event& e){
    const EventKind k = eventKind(e);
    if(k == EV_PRESS || k == EV_RELEASE) return e.b1 < KEYS;
    return k != EV_UNKNOWN;
}

} // namespace

bool Action::sameAs(const Action& o) const {
    if(kind != o.kind) return false;
    switch(kind){
    case NATIVE: return true;
    case REMAP: return dst == o.dst;
    case MACRO: return subtype == o.subtype && start == o.start && run == o.run && repeat == o.repeat && events == o.events;
    }
    return false;
}

EventKind eventKind(const Event& e){
    if(e.b0 == 0x20) return EV_PRESS;
    if(e.b0 == 0x00) return EV_RELEASE;
    if(e.b0 >= 0x80 && e.b0 <= 0x9f) return EV_DELAY;
    if(e.b0 >= 0xa0 && e.b0 <= 0xbf) return EV_ODD_SHORT;
    if(e.b0 == 0xc0) return EV_LONG;
    if(e.b0 == 0xe0) return EV_RANDOM;
    return EV_UNKNOWN;
}

Event press(uint8_t key){ Event e; e.b0 = 0x20; e.b1 = key; return e; }
Event release(uint8_t key){ Event e; e.b0 = 0x00; e.b1 = key; return e; }

Event delay(unsigned ms){
    Event e;
    if(ms > DELAY_LONG_MAX) ms = DELAY_LONG_MAX;
    if(ms <= DELAY_SHORT_MAX){
        e.b0 = static_cast<uint8_t>(0x80 | (ms >> 8));
        e.b1 = static_cast<uint8_t>(ms & 0xff);
    } else {
        e.b0 = 0xc0;
        e.b1 = static_cast<uint8_t>(ms >> 16);
        e.b2 = static_cast<uint8_t>(ms >> 8);
        e.b3 = static_cast<uint8_t>(ms);
    }
    return e;
}

Event randomDelay(unsigned min, unsigned max){
    if(max > RANDOM_MAX) max = RANDOM_MAX;
    if(min > max) min = max;
    Event e;
    e.b0 = 0xe0;
    e.b1 = static_cast<uint8_t>(min >> 4);
    e.b2 = static_cast<uint8_t>(((min & 0x0f) << 4) | (max >> 8));
    e.b3 = static_cast<uint8_t>(max);
    return e;
}

unsigned delayMs(const Event& e){
    switch(eventKind(e)){
    case EV_DELAY:
    case EV_ODD_SHORT:
        return unsigned(e.b0 & 0x1f) << 8 | e.b1;
    case EV_LONG:
        return unsigned(e.b1) << 16 | unsigned(e.b2) << 8 | e.b3;
    default:
        return 0;
    }
}

unsigned randomMin(const Event& e){ return unsigned(e.b1) << 4 | (e.b2 >> 4); }
unsigned randomMax(const Event& e){ return unsigned(e.b2 & 0x0f) << 8 | e.b3; }

bool sameBindings(const Keys& a, const Keys& b){
    for(unsigned k = 0; k < KEYS; ++k)
        if(!a[k].sameAs(b[k])) return false;
    return true;
}

std::string encode(const std::string& bytes){
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for(char ch : bytes){
        const unsigned char c = static_cast<unsigned char>(ch);
        if(unreserved(c)) out.push_back(ch);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool decode(const std::string& text, std::string& out){
    out.clear();
    for(size_t i = 0; i < text.size(); ++i){
        unsigned char c = static_cast<unsigned char>(text[i]);
        if(c == '%'){
            if(i + 2 >= text.size()) return false;
            const int hi = upper_hex(text[i + 1]), lo = upper_hex(text[i + 2]);
            if(hi < 0 || lo < 0) return false;
            c = static_cast<unsigned char>(hi << 4 | lo);
            if(unreserved(c)) return false;   // one text, one encoding
            i += 2;
        } else if(!unreserved(c)) return false;
        out.push_back(static_cast<char>(c));
    }
    return true;
}

bool validUtf8(const std::string& s, size_t* units){
    size_t n = 0;
    for(size_t i = 0; i < s.size();){
        const unsigned char b = static_cast<unsigned char>(s[i]);
        unsigned need;
        uint32_t c;
        if(b < 0x80){ need = 1; c = b; }
        else if(b >= 0xc2 && b < 0xe0){ need = 2; c = b & 0x1f; }
        else if(b >= 0xe0 && b < 0xf0){ need = 3; c = b & 0x0f; }
        else if(b >= 0xf0 && b < 0xf5){ need = 4; c = b & 0x07; }
        else return false;
        if(need > s.size() - i) return false;
        for(unsigned j = 1; j < need; ++j){
            const unsigned char x = static_cast<unsigned char>(s[i + j]);
            if((x & 0xc0) != 0x80) return false;
            c = (c << 6) | (x & 0x3f);
        }
        if(c == 0 || (need == 3 && c < 0x800) || (need == 4 && (c < 0x10000 || c > 0x10ffff)) || (c >= 0xd800 && c < 0xe000))
            return false;
        n += c >= 0x10000 ? 2 : 1;
        i += need;
    }
    if(units) *units = n;
    return true;
}

std::vector<std::string> cutText(const std::string& utf8){
    std::vector<std::string> pieces;
    std::string cur;
    size_t curenc = 0;
    for(size_t i = 0; i < utf8.size();){
        size_t k = 1;
        while(i + k < utf8.size() && (static_cast<unsigned char>(utf8[i + k]) & 0xc0) == 0x80) ++k;
        const std::string ch = utf8.substr(i, k);
        const size_t e = encode(ch).size();
        if(curenc + e > TEXT_MAX){
            pieces.push_back(cur);
            cur.clear();
            curenc = 0;
        }
        cur += ch;
        curenc += e;
        i += k;
    }
    pieces.push_back(cur);
    return pieces;
}

bool parsePage(const std::string& line, Page& out, std::string& err){
    out = Page();
    if(line.size() + 1 > LINE_BYTES){ err = "a line longer than the protocol allows"; return false; }
    const std::vector<std::string> w = split(line, ' ', static_cast<size_t>(-1));
    unsigned long m, gen, p, pages;
    if(w.size() < 5 || w[0] != "mode" || !dec(w[1], 3, m) || m < 1 || w[2] != "hwbind" || !dec(w[3], 0xffffffffUL, gen)){
        err = "not an answer to get :hwbind";
        return false;
    }
    out.mode = static_cast<unsigned>(m);
    out.gen = gen;
    if(w[4] == "error"){
        if(w.size() != 6 || (w[5] != "nocache" && w[5] != "range")){ err = "a malformed error line"; return false; }
        out.error = true;
        out.why = w[5];
        return true;
    }
    const std::vector<std::string> pp = split(w[4], '/', 2);
    if(pp.size() != 2 || !dec(pp[0], 0xffffffffUL, p) || !dec(pp[1], 0xffffffffUL, pages) || p < 1 || p > pages || w.size() < 6){
        err = "a malformed page";
        return false;
    }
    out.page = static_cast<unsigned>(p);
    out.pages = static_cast<unsigned>(pages);
    for(size_t i = 5; i < w.size(); ++i){
        if(w[i].empty() || w[i].size() > WORD_MAX){ err = "a malformed page"; return false; }
        out.words.push_back(w[i]);
    }
    return true;
}

bool parseRecord(const std::vector<std::string>& w, Record& out, std::string& err){
    out = Record();
    size_t i = 0;
    auto next = [&](const char* prefix) -> bool { return i < w.size() && starts(w[i], prefix); };
    if(!next("v:1") || w[i] != "v:1"){ err = "not a record of version 1"; return false; }
    ++i;
    if(!next("id:") || !valid_id(w[i].substr(3))){ err = "a malformed id"; return false; }
    out.id = w[i++].substr(3);
    if(!next("st:")){ err = "no state"; return false; }
    const std::vector<std::string> st = split(w[i].substr(3), ':', 2);
    if(st[0] == "ok" && st.size() == 1) out.state = Record::OK;
    else if(st[0] == "empty" && st.size() == 1) out.state = Record::EMPTY;
    else if((st[0] == "raw" || st[0] == "broken") && st.size() == 2 && st[1].size() <= TEXT_MAX && decode(st[1], out.reason)
            && validUtf8(out.reason))
        out.state = st[0] == "raw" ? Record::RAW : Record::BROKEN;
    else { err = "a malformed state"; return false; }
    ++i;
    if((out.state == Record::EMPTY) != (out.id == "0")){ err = "an id that does not go with the state"; return false; }
    if(next("ro:") && out.state != Record::EMPTY){
        const std::string t = w[i].substr(3);
        if(t.size() > TEXT_MAX || !decode(t, out.readonlyWhy) || !validUtf8(out.readonlyWhy)){ err = "a malformed ro word"; return false; }
        out.readonly = true;
        ++i;
    }
    if(out.state == Record::EMPTY){
        if(w.size() != 6 || w[3] != "lt:empty" || w[4] != "ly:0" || w[5] != "end"){ err = "a malformed empty record"; return false; }
        out.light = Record::NONE;
        return true;
    }
    if(next("nm:")){
        if(!name_words(w, i, "nm:", out.name, err)) return false;
        out.hasName = true;
    }
    uint32_t v;
    if(next("wl:")){
        if(!hex_exact(w[i].substr(3), 2, v)){ err = "a malformed wl word"; return false; }
        out.hasWinlock = true;
        out.winlock = static_cast<uint8_t>(v);
        ++i;
    }
    if(next("ind:")){
        const std::string h = w[i].substr(4);
        if(h.size() != 24){ err = "a malformed ind word"; return false; }
        for(unsigned j = 0; j < 12; ++j){
            if(!hex_exact(h.substr(2 * j, 2), 2, v)){ err = "a malformed ind word"; return false; }
            out.indicators[j] = static_cast<uint8_t>(v);
        }
        out.hasIndicators = true;
        ++i;
    }
    if(!next("lt:")){ err = "no lt word"; return false; }
    const std::string lt = w[i++].substr(3);
    if(lt == "static") out.light = Record::STATIC;
    else if(lt == "empty") out.light = Record::NONE;
    else if(lt == "effects") out.light = Record::EFFECTS;
    else if(lt == "unknown") out.light = Record::UNKNOWN;
    else { err = "a malformed lt word"; return false; }
    unsigned long layers;
    if(!next("ly:") || !dec(w[i].substr(3), 0xffffffffUL, layers)){ err = "a malformed ly word"; return false; }
    out.layers = static_cast<unsigned>(layers);
    ++i;
    int last = -1;
    while(next("m:")){
        const std::vector<std::string> f = split(w[i].substr(2), ':', 3);
        uint32_t k, d;
        if(out.state != Record::OK || f.size() != 2 || !hex_exact(f[0], 2, k) || k >= KEYS || !hex_exact(f[1], 2, d) || !valid_dst(d)
           || static_cast<int>(k) <= last){
            err = "a malformed m word";
            return false;
        }
        last = static_cast<int>(k);
        out.keys[k].kind = Action::REMAP;
        out.keys[k].dst = static_cast<uint8_t>(d);
        ++i;
    }
    last = -1;
    while(next("a:")){
        const std::vector<std::string> f = split(w[i].substr(2), ':', 9);
        uint32_t k, sub, start, run, rep, flags;
        unsigned long nev;
        if(out.state != Record::OK || f.size() != 8 || !hex_exact(f[0], 2, k) || k >= KEYS || static_cast<int>(k) <= last
           || out.keys[k].kind != Action::NATIVE || (f[1] != "-" && (f[1].size() != 4 || f[1][0] != 'M' || f[1].find_first_not_of("0123456789abcdef", 1) != std::string::npos))
           || !hex_exact(f[2], 2, sub) || !hex_exact(f[3], 2, start) || !hex_exact(f[4], 2, run) || !hex_exact(f[5], 2, rep)
           || !dec(f[6], 131072, nev) || !hex_exact(f[7], 2, flags)){
            err = "a malformed a word";
            return false;
        }
        last = static_cast<int>(k);
        Action& a = out.keys[k];
        a.kind = Action::MACRO;
        a.file = f[1] == "-" ? std::string() : f[1];
        a.subtype = static_cast<uint8_t>(sub);
        a.start = static_cast<uint8_t>(start);
        a.run = static_cast<uint8_t>(run);
        a.repeat = static_cast<uint8_t>(rep);
        a.flags = flags;
        ++i;
        bool fourNext = false;   // the last e: word stopped at 126 bytes: the next one starts with an event of 4
        while(a.events.size() < nev){
            const std::vector<std::string> e = next("e:") ? split(w[i].substr(2), ':', 3) : std::vector<std::string>();
            uint32_t ek;
            unsigned long off;
            if(e.size() != 3 || !hex_exact(e[0], 2, ek) || ek != k || !dec(e[1], 131072, off) || off != a.events.size() || e[2].empty()
               || e[2].size() % 4){
                err = "the events of a macro are not all there";
                return false;
            }
            // Each event 4 or 8 hex digits by its first byte; a word that is not the last holds 126 or 128 bytes, 126 only before
            // an event of 4 bytes (cape_hwslot.h)
            size_t bytes = 0;
            for(size_t at = 0; at < e[2].size();){
                uint32_t b0, rest;
                if(!hex_exact(e[2].substr(at, 2), 2, b0)){ err = "a malformed e word"; return false; }
                Event ev;
                ev.b0 = static_cast<uint8_t>(b0);
                const size_t size = ev.size();
                if(at + 2 * size > e[2].size() || !hex_exact(e[2].substr(at + 2, 2 * size - 2), 2 * size - 2, rest)
                   || a.events.size() >= nev || (bytes == 0 && fourNext && size != 4)){
                    err = "a malformed e word";
                    return false;
                }
                ev.b1 = static_cast<uint8_t>(size == 4 ? rest >> 16 : rest);
                if(size == 4){ ev.b2 = static_cast<uint8_t>(rest >> 8); ev.b3 = static_cast<uint8_t>(rest); }
                a.events.push_back(ev);
                bytes += size;
                at += 2 * size;
            }
            const bool lastWord = a.events.size() == nev;
            if(bytes > EVENT_BYTES || (!lastWord && bytes < EVENT_BYTES - 2)){
                err = "a malformed e word";
                return false;
            }
            fourNext = !lastWord && bytes == EVENT_BYTES - 2;
            ++i;
        }
    }
    if(i + 1 != w.size() || w[i] != "end"){ err = "the record does not end where it should"; return false; }
    return true;
}

std::vector<std::string> stageWords(const Stage& s){
    std::vector<std::string> w;
    uint32_t txn;
    size_t units = 0;
    if(!hex_exact(s.txn, 8, txn) || !txn || s.mode < 1 || s.mode > 3 || !valid_id(s.base)
       || (s.hasName && (s.name.empty() || !validUtf8(s.name, &units) || units > NAME_UNITS)))
        return w;
    const std::string& x = s.txn;
    char buf[64];
    std::snprintf(buf, sizeof(buf), ":%u:", s.mode);
    w.push_back("begin:" + x + buf + s.base);
    if(s.hasName){
        const std::vector<std::string> pieces = cutText(s.name);
        for(size_t i = 0; i < pieces.size(); ++i){
            std::snprintf(buf, sizeof(buf), ":%u/%u:", static_cast<unsigned>(i + 1), static_cast<unsigned>(pieces.size()));
            w.push_back("name:" + x + buf + encode(pieces[i]));
        }
    }
    w.push_back("light:" + x + (s.keepLight ? ":keep" : ":pic"));
    if(!s.keepLight){
        std::string cur;
        for(const auto& led : s.rgb){       // std::map: ascending byte order of name
            if(!valid_led(led.first) || led.second > 0xffffff) return std::vector<std::string>();
            if(!led.second) continue;
            std::snprintf(buf, sizeof(buf), "=%06x", static_cast<unsigned>(led.second));
            const std::string item = led.first + buf;
            if(!cur.empty() && cur.size() + 1 + item.size() <= WORD_MAX) cur += "," + item;
            else {
                if(!cur.empty()) w.push_back(cur);
                cur = "rgb:" + x + ":" + item;
            }
        }
        if(!cur.empty()) w.push_back(cur);
    }
    if(s.hasWinlock){
        std::snprintf(buf, sizeof(buf), ":%02x", s.winlock);
        w.push_back("wl:" + x + buf);
    }
    if(s.hasIndicators){
        std::string hex;
        for(uint8_t v : s.indicators){ std::snprintf(buf, sizeof(buf), "%02x", v); hex += buf; }
        w.push_back("ind:" + x + ":" + hex);
    }
    if(s.keepBindings) w.push_back("bind:" + x + ":keep");
    else {
        std::snprintf(buf, sizeof(buf), ":model");
        w.push_back("bind:" + x + buf + (s.recreate ? ":recreate" : ""));
        for(unsigned k = 0; k < KEYS; ++k){
            if(s.keys[k].kind != Action::REMAP) continue;
            if(!valid_dst(s.keys[k].dst)) return std::vector<std::string>();
            std::snprintf(buf, sizeof(buf), ":%02x:%02x", k, s.keys[k].dst);
            w.push_back("map:" + x + buf);
        }
        for(unsigned k = 0; k < KEYS; ++k){
            const Action& a = s.keys[k];
            if(a.kind != Action::MACRO) continue;
            for(const Event& e : a.events)
                if(!event_known(e)) return std::vector<std::string>();
            std::snprintf(buf, sizeof(buf), ":%02x:%02x:%02x:%02x:%02x:%u", k, a.subtype, a.start, a.run, a.repeat,
                          static_cast<unsigned>(a.events.size()));
            w.push_back("act:" + x + buf);
            add_events(w, "ev:" + x, k, a.events);
        }
    }
    std::snprintf(buf, sizeof(buf), ":%u", static_cast<unsigned>(w.size()));
    w.push_back("end:" + x + buf);
    return w;
}

std::vector<std::string> packLines(unsigned owner, const std::vector<std::string>& words){
    std::vector<std::string> lines;
    std::string cur;
    char head[32];
    std::snprintf(head, sizeof(head), "@%u hwslot ", owner);
    for(const std::string& word : words){
        if(!cur.empty() && cur.size() + 1 + word.size() + 1 <= LINE_BYTES) cur += " " + word;
        else {
            if(!cur.empty()) lines.push_back(cur);
            cur = head + word;
        }
    }
    if(!cur.empty()) lines.push_back(cur);
    return lines;
}

namespace {
std::string request(const char* verb, unsigned owner, const Stage& s){
    char buf[64];
    std::snprintf(buf, sizeof(buf), "@%u hwslot %s:%s:%u", owner, verb, s.txn.c_str(), s.mode);
    return buf;
}
} // namespace

std::string checkLine(unsigned owner, const Stage& s){ return request("check", owner, s); }
std::string saveLine(unsigned owner, const Stage& s){ return request("save", owner, s); }
std::string abortLine(unsigned owner, const Stage& s){ return request("abort", owner, s); }

} // namespace HwBinding
