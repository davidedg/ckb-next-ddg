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
    d.hasBindings = record.state == HwBinding::Record::OK || record.state == HwBinding::Record::EMPTY;
    if(d.hasBindings)
        d.keys = record.keys;
    // the performance settings whatever the bindings are (RAW and BROKEN ones too)
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

// v:3 base:<id|-> mod:<0|1> hb:<0|1> wl:<hh|-> ind:<24 hex|-> rc:<0|1> rl:<0|1>, then m:<key>:<dst> and
// a:<key>:<sub>:<start>:<run>:<rep>:<events> (each event as in the file: four or eight hex digits by its first byte) in
// ascending order of key. v:2 (an earlier version) had four hex digits per event, the delays in 15 bits: it is read so, and a delay of
// 16 384 ms or more (first byte c0..ff, which is the start of a 4-byte event now) becomes the long delay of iCUE's form; a0..bf
// stays as it is (a 2-byte delay iCUE reads in 13 bits, kept). "-" is a value the draft does not
// know. The conflict is not kept: the next record decides again. v:1 (before the performance settings) had wl:<hh> and no ind: it is read with neither
// known, and its wl: is not taken (that GUI had no editor of the Win Lock options, and wrote 01 for bindings that were not a model)
std::string serialize(const Draft& d){
    std::ostringstream out;
    out << "v:3 base:" << (d.base.empty() ? "-" : d.base) << " mod:" << (d.modified ? 1 : 0) << " hb:" << (d.hasBindings ? 1 : 0)
        << " wl:";
    if(d.hasWinlock) hex2(out, d.winlock);
    else out << '-';
    out << " ind:";
    if(d.hasIndicators) for(uint8_t v : d.indicators) hex2(out, v);
    else out << '-';
    out << " rc:" << (d.recreate ? 1 : 0) << " rl:" << (d.replaceLights ? 1 : 0);
    if(!d.hasBindings)
        return out.str();
    for(unsigned k = 0; k < HwBinding::KEYS; ++k){
        const HwBinding::Action& a = d.keys[k];
        if(a.kind == HwBinding::Action::REMAP){
            out << " m:"; hex2(out, k); out << ':'; hex2(out, a.dst);
        } else if(a.kind == HwBinding::Action::MACRO){
            out << " a:"; hex2(out, k);
            for(unsigned v : {unsigned(a.subtype), unsigned(a.start), unsigned(a.run), unsigned(a.repeat)}){ out << ':'; hex2(out, v); }
            out << ':';
            for(const HwBinding::Event& e : a.events){
                hex2(out, e.b0); hex2(out, e.b1);
                if(e.size() == 4){ hex2(out, e.b2); hex2(out, e.b3); }
            }
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
    const bool v1 = !w.empty() && w[0] == "v:1";
    const bool v3 = !w.empty() && w[0] == "v:3";
    const size_t fixed = v1 ? 7 : 8;   // the words before the bindings
    bool ok = w.size() >= fixed && (v1 || v3 || w[0] == "v:2") && w[1].compare(0, 5, "base:") == 0 && w[1].size() > 5 &&
              flag(w[2], "mod", r.modified) && flag(w[3], "hb", r.hasBindings) && w[4].compare(0, 3, "wl:") == 0 &&
              flag(w[fixed - 2], "rc", r.recreate) && flag(w[fixed - 1], "rl", r.replaceLights);
    unsigned v = 0;
    if(ok){
        r.base = w[1].substr(5) == "-" ? std::string() : w[1].substr(5);
        if(v1)
            ok = w[4].size() == 5 && byte(w[4], 3, v);   // read, and not taken
        else if(w[4] != "wl:-"){
            ok = w[4].size() == 5 && byte(w[4], 3, v);
            r.hasWinlock = true;
            r.winlock = uint8_t(v);
        }
    }
    if(ok && !v1 && w[5] != "ind:-"){
        ok = w[5].size() == 4 + 2 * r.indicators.size() && w[5].compare(0, 4, "ind:") == 0;
        for(size_t j = 0; ok && j < r.indicators.size(); ++j){
            ok = byte(w[5], 4 + 2 * j, v);
            r.indicators[j] = uint8_t(v);
        }
        r.hasIndicators = ok;
    }
    if(ok && !r.hasBindings && w.size() != fixed) ok = false;
    int last = -1;
    for(size_t i = fixed; ok && i < w.size(); ++i){
        const std::vector<std::string> f = split(w[i], ':');
        unsigned key = 0;
        ok = f.size() >= 3 && f[1].size() == 2 && byte(f[1], 0, key) && key < HwBinding::KEYS && int(key) > last;
        if(!ok) break;
        last = int(key);
        HwBinding::Action& a = r.keys[key];
        if(f[0] == "m" && f.size() == 3){
            unsigned dst = 0;
            ok = f[2].size() == 2 && byte(f[2], 0, dst);
            a.kind = HwBinding::Action::REMAP;
            a.dst = uint8_t(dst);
        } else if(f[0] == "a" && f.size() == 7){
            unsigned field[4];
            for(int j = 0; ok && j < 4; ++j) ok = f[size_t(j) + 2].size() == 2 && byte(f[size_t(j) + 2], 0, field[j]);
            ok = ok && f[6].size() % 4 == 0;
            if(!ok) break;
            a.kind = HwBinding::Action::MACRO;
            a.subtype = uint8_t(field[0]); a.start = uint8_t(field[1]); a.run = uint8_t(field[2]); a.repeat = uint8_t(field[3]);
            for(size_t e = 0; ok && e < f[6].size();){
                unsigned b0 = 0, b1 = 0;
                ok = byte(f[6], e, b0) && byte(f[6], e + 2, b1);
                if(!ok) break;
                HwBinding::Event ev;
                ev.b0 = uint8_t(b0); ev.b1 = uint8_t(b1);
                if(!v3){
                    // a 2-byte delay of 15 bits; from 16 384 ms its first byte would read as a 4-byte event: iCUE's long form
                    if(b0 >= 0xc0) ev = HwBinding::delay((b0 & 0x7f) << 8 | b1);
                    e += 4;
                } else if(ev.size() == 4){
                    unsigned b2 = 0, b3 = 0;
                    ok = e + 8 <= f[6].size() && byte(f[6], e + 4, b2) && byte(f[6], e + 6, b3);
                    ev.b2 = uint8_t(b2); ev.b3 = uint8_t(b3);
                    e += 8;
                } else
                    e += 4;
                if(ok) a.events.push_back(ev);
            }
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
