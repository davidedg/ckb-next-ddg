#include "hwslotdraft.h"

#include <cstdio>
#include <sstream>
#include <vector>

namespace HwSlotDraft {

Apply decide(const Draft& d, const std::string& newId, bool committing){
    if(committing || !d.modified)
        return REPLACE;
    if(d.base == newId)
        return KEEP;
    return CONFLICT;
}

void replace(Draft& d, const HwBinding::Record& record, bool hasRecord){
    d = Draft();
    if(!hasRecord)
        return;
    d.base = record.id;
    d.lighting = record.lighting;
    d.baseLighting = record.lighting;
    d.hasBindings = record.state == HwBinding::Record::OK || record.state == HwBinding::Record::EMPTY;
    if(d.hasBindings)
        d.keys = record.keys;
    // the performance settings as the record has them (a slot that cannot be rewritten has its indicator colours, if any)
    d.hasWinlock = record.hasWinlock;
    d.winlock = record.hasWinlock ? record.winlock : 0x01;
    d.hasIndicators = record.hasIndicators;
    if(record.hasIndicators)
        d.indicators = record.indicators;
}

Perf basePerf(const HwBinding::Record* record){
    Perf p;
    if(record && record->hasWinlock)
        p.winlock = record->winlock;
    if(record && record->hasIndicators)
        p.indicators = record->indicators;
    return p;
}

Perf effectivePerf(const Draft& d, const HwBinding::Record* fallback){
    Perf p = basePerf(fallback);
    if(d.hasWinlock)
        p.winlock = d.winlock;
    if(d.hasIndicators)
        p.indicators = d.indicators;
    return p;
}

void keep(Draft& d, const std::string& newId){
    d.base = newId;
    d.conflict = false;
}

namespace {

void hex2(std::ostringstream& out, unsigned v){
    char b[3];
    std::snprintf(b, sizeof(b), "%02x", v & 0xff);
    out << b;
}

bool hexval(char c, unsigned& v){
    if(c >= '0' && c <= '9') v = unsigned(c - '0');
    else if(c >= 'a' && c <= 'f') v = unsigned(c - 'a' + 10);
    else return false;
    return true;
}

bool byte(const std::string& s, size_t at, unsigned& out){
    unsigned hi, lo;
    if(at + 2 > s.size() || !hexval(s[at], hi) || !hexval(s[at + 1], lo)) return false;
    out = hi << 4 | lo;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep){
    std::vector<std::string> out;
    std::string cur;
    for(char c : s){
        if(c == sep){ out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

bool flag(const std::string& word, const char* name, bool& out){
    const std::string prefix = std::string(name) + ":";
    if(word != prefix + "0" && word != prefix + "1") return false;
    out = word.back() == '1';
    return true;
}

} // namespace

// Unversioned experimental format. The two complete lighting bodies precede the binding words.
// Old drafts are test material and are intentionally rejected, without migration or backup.
std::string serialize(const Draft& d){
    std::ostringstream out;
    out << "base:" << (d.base.empty() ? "-" : d.base) << " mod:" << (d.modified ? 1 : 0) << " hb:" << (d.hasBindings ? 1 : 0)
        << " wl:";
    if(d.hasWinlock) hex2(out, d.winlock);
    else out << '-';
    out << " ind:";
    if(d.hasIndicators) for(uint8_t v : d.indicators) hex2(out, v);
    else out << '-';
    std::vector<std::string> lighting, baseLighting;
    std::string error;
    if(!HwLighting::write(d.lighting, lighting, error) || !HwLighting::write(d.baseLighting, baseLighting, error)) return {};
    out << " base-light";
    for(const auto& word : baseLighting) out << ' ' << word;
    out << " draft-light";
    for(const auto& word : lighting) out << ' ' << word;
    if(!d.hasBindings)
        return out.str();
    for(unsigned k = 0; k < HwBinding::KEYS; ++k){
        const HwBinding::Action& a = d.keys[k];
        if(a.kind == HwBinding::Action::REMAP){
            out << " m:"; hex2(out, k); out << ':'; hex2(out, a.dst);
            if(!a.shape.empty()) out << ":bs=" << HwBinding::encode(a.shape);
        } else if(a.kind == HwBinding::Action::MACRO){
            out << " a:"; hex2(out, k);
            for(unsigned v : {unsigned(a.subtype), unsigned(a.start), unsigned(a.run), unsigned(a.repeat)}){ out << ':'; hex2(out, v); }
            out << ':';
            for(const HwBinding::Event& e : a.events){
                hex2(out, e.b0); hex2(out, e.b1);
                if(e.size() == 4){ hex2(out, e.b2); hex2(out, e.b3); }
            }
            if(a.imitateMs) out << ":im=" << a.imitateMs;
            if(!a.text.empty()) out << ":tx=" << HwBinding::encode(a.text);
            if(!a.shape.empty()) out << ":bs=" << HwBinding::encode(a.shape);
        }
    }
    return out.str();
}

bool deserialize(const std::string& line, Draft& d){
    Draft r;
    std::istringstream in(line);
    std::vector<std::string> w;
    std::string word;
    while(in >> word) w.push_back(word);
    const size_t fixed = 6;
    bool ok = w.size() > fixed && w[0].compare(0, 5, "base:") == 0 && w[0].size() > 5 &&
              flag(w[1], "mod", r.modified) && flag(w[2], "hb", r.hasBindings) && w[3].compare(0, 3, "wl:") == 0 && w[5] == "base-light";
    unsigned v = 0;
    if(ok){
        r.base = w[0].substr(5) == "-" ? std::string() : w[0].substr(5);
        ok = r.base.empty() || HwBinding::validId(r.base);
        if(w[3] != "wl:-"){
            ok = ok && w[3].size() == 5 && byte(w[3], 3, v);
            r.hasWinlock = true;
            r.winlock = uint8_t(v);
        }
    }
    if(ok && w[4] != "ind:-"){
        ok = w[4].size() == 4 + 2 * r.indicators.size() && w[4].compare(0, 4, "ind:") == 0;
        for(size_t j = 0; ok && j < r.indicators.size(); ++j){
            ok = byte(w[4], 4 + 2 * j, v);
            r.indicators[j] = uint8_t(v);
        }
        r.hasIndicators = ok;
    }
    size_t bindings = fixed;
    std::string error;
    if(ok) ok = HwLighting::read(w, bindings, r.baseLighting, error);
    if(ok) ok = bindings < w.size() && w[bindings++] == "draft-light";
    if(ok) ok = HwLighting::read(w, bindings, r.lighting, error);
    if(ok && !r.hasBindings && w.size() != bindings) ok = false;
    int last = -1;
    for(size_t i = bindings; ok && i < w.size(); ++i){
        std::vector<std::string> f = split(w[i], ':');
        std::string shape;
        if(!f.empty() && f.back().compare(0, 3, "bs=") == 0){
            ok = HwBinding::decode(f.back().substr(3), shape) && !shape.empty() && shape.size() <= HwBinding::SHAPE_WORDS * HwBinding::TEXT_MAX;
            if(!ok) break;
            f.pop_back();
        }
        unsigned key = 0;
        ok = f.size() >= 3 && f[1].size() == 2 && byte(f[1], 0, key) && key < HwBinding::KEYS && int(key) > last;
        if(!ok) break;
        last = int(key);
        HwBinding::Action& a = r.keys[key];
        a.shape = shape;
        if(f[0] == "m" && f.size() == 3){
            unsigned dst = 0;
            ok = f[2].size() == 2 && byte(f[2], 0, dst);
            a.kind = HwBinding::Action::REMAP;
            a.dst = uint8_t(dst);
        } else if(f[0] == "a" && f.size() >= 7 && f.size() <= 9){
            unsigned field[4];
            for(int j = 0; ok && j < 4; ++j) ok = f[size_t(j) + 2].size() == 2 && byte(f[size_t(j) + 2], 0, field[j]);
            ok = ok && f[6].size() % 4 == 0;
            // im= and then tx=, each at most once
            for(size_t j = 7; ok && j < f.size(); ++j){
                if(f[j].compare(0, 3, "im=") == 0 && j == 7 && !a.imitateMs){
                    unsigned long ms = 0;
                    const std::string n = f[j].substr(3);
                    ok = !n.empty() && n.size() <= 9 && n.find_first_not_of("0123456789") == std::string::npos && n[0] != '0';
                    if(ok) ms = std::stoul(n);
                    ok = ok && ms > HwBinding::IMITATE_FILE_MAX && ms <= HwBinding::IMITATE_MAX;
                    a.imitateMs = ms;
                } else if(f[j].compare(0, 3, "tx=") == 0 && j + 1 == f.size())
                    ok = f[j].size() > 3 && HwBinding::decode(f[j].substr(3), a.text) && HwBinding::validUtf8(a.text);
                else
                    ok = false;
            }
            if(!ok) break;
            a.kind = HwBinding::Action::MACRO;
            a.subtype = uint8_t(field[0]); a.start = uint8_t(field[1]); a.run = uint8_t(field[2]); a.repeat = uint8_t(field[3]);
            for(size_t e = 0; ok && e < f[6].size();){
                unsigned b0 = 0, b1 = 0;
                ok = byte(f[6], e, b0) && byte(f[6], e + 2, b1);
                if(!ok) break;
                HwBinding::Event ev;
                ev.b0 = uint8_t(b0); ev.b1 = uint8_t(b1);
                if(ev.size() == 4){
                    unsigned b2 = 0, b3 = 0;
                    ok = e + 8 <= f[6].size() && byte(f[6], e + 4, b2) && byte(f[6], e + 6, b3);
                    ev.b2 = uint8_t(b2); ev.b3 = uint8_t(b3);
                    e += 8;
                } else
                    e += 4;
                if(ok) a.events.push_back(ev);
            }
            // an interval only for the longest Imitate, a string only for a Text
            ok = ok && (!a.imitateMs || a.longestImitate()) && (a.text.empty() || a.subtype == HwBinding::SUBTYPE_TEXT);
        } else
            ok = false;
    }
    if(!ok){
        d = Draft();
        return false;
    }
    d = r;
    return true;
}

} // namespace HwSlotDraft
