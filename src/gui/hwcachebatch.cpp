#include "hwcachebatch.h"

#include <limits>
#include <sstream>
#include <vector>

namespace {
std::vector<std::string> words(const std::string& line){
    std::istringstream input(line);
    std::vector<std::string> out;
    std::string word;
    while(input >> word) out.push_back(word);
    return out;
}

bool hex(char c){
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool valid_guid(const std::string& value){
    if(value.size() != 38 || value.front() != '{' || value.back() != '}') return false;
    for(size_t i = 1; i < 37; ++i){
        if(i == 9 || i == 14 || i == 19 || i == 24){
            if(value[i] != '-') return false;
        } else if(!hex(value[i])) return false;
    }
    return true;
}

bool valid_revision(const std::string& value){
    if(value.empty() || value.size() > 8) return false;
    for(char c : value) if(!hex(c)) return false;
    return true;
}

int hex_value(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

bool valid_utf8(const std::string& value){
    for(size_t i = 0; i < value.size();){
        const unsigned char a = static_cast<unsigned char>(value[i]);
        unsigned count, code, minimum;
        if(a < 0x80){ count = 1; code = a; minimum = 0; }
        else if((a & 0xe0) == 0xc0){ count = 2; code = a & 0x1f; minimum = 0x80; }
        else if((a & 0xf0) == 0xe0){ count = 3; code = a & 0x0f; minimum = 0x800; }
        else if((a & 0xf8) == 0xf0){ count = 4; code = a & 0x07; minimum = 0x10000; }
        else return false;
        if(count > value.size() - i) return false;
        for(unsigned j = 1; j < count; ++j){
            const unsigned char b = static_cast<unsigned char>(value[i + j]);
            if((b & 0xc0) != 0x80) return false;
            code = (code << 6) | (b & 0x3f);
        }
        if(code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += count;
    }
    return true;
}

bool valid_name(const std::string& value){
    if(value.empty()) return false;
    std::string decoded;
    decoded.reserve(value.size());
    for(size_t i = 0; i < value.size(); ++i){
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if(c == '%'){
            if(i + 2 >= value.size() || !hex(value[i + 1]) || !hex(value[i + 2])) return false;
            const char byte = static_cast<char>((hex_value(value[i + 1]) << 4) | hex_value(value[i + 2]));
            if(byte == '\0') return false;
            decoded.push_back(byte);
            i += 2;
        } else {
            if(c <= 0x20 || c >= 0x7f || c == ':' || c == '@') return false;
            decoded.push_back(static_cast<char>(c));
        }
    }
    return valid_utf8(decoded);
}

bool valid_rgb(const std::vector<std::string>& w){
    if(w.size() < 4) return false;
    for(size_t i = 3; i < w.size(); ++i){
        const std::string& token = w[i];
        const size_t colon = token.find(':');
        const std::string colour = colon == std::string::npos ? token : token.substr(colon + 1);
        if(colour.size() != 6) return false;
        for(char c : colour) if(!hex(c)) return false;
        if(colon != std::string::npos){
            if(colon == 0 || token.find(':', colon + 1) != std::string::npos) return false;
            for(size_t j = 0; j < colon; ++j){
                const char c = token[j];
                if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ','))
                    return false;
                if(c == ',' && (j == 0 || j + 1 == colon || token[j - 1] == ',')) return false;
            }
        }
    }
    return true;
}

bool mode_count(const std::string& token, unsigned& count){
    if(token.empty()) return false;
    count = 0;
    for(char c : token){
        if(c < '0' || c > '9') return false;
        const unsigned digit = static_cast<unsigned>(c - '0');
        if(count > (static_cast<unsigned>(std::numeric_limits<int>::max()) - digit) / 10) return false;
        count = count * 10 + digit;
    }
    return count >= 3;
}
}

HwCacheBatch::HwCacheBatch(unsigned mask, unsigned rgbRequired, bool requireChangedIds)
    : HwCacheBatch(mask, rgbRequired, requireChangedIds, Result()){}

HwCacheBatch::HwCacheBatch(unsigned mask, unsigned rgbRequired, bool requireChangedIds,
                           const Result& previous)
    : mask_(mask), rgbRequired_(rgbRequired), requireChangedIds_(requireChangedIds),
      previous_(previous), pending_(previous){
    if(mask == 0 || (mask & ~7u) || (rgbRequired & ~mask)) fail("invalid mask");
    for(unsigned i = 0; i < 3; ++i)
        if(mask_ & (1u << i)){
            pending_.modes[i].rgb.clear();
            pending_.modes[i].hasRgb = false;
            pending_.modes[i].hasRecord = false;
            pending_.modes[i].gen = 0;
            pending_.modes[i].record = HwBinding::Record();
        }
}

void HwCacheBatch::requestBindings(){
    if(state_ == Identity && !profileIdSeen_ && !modeIdsSeen_) bindings_ = true;
}

std::string HwCacheBatch::takeRequest(unsigned notify){
    if(!requestReady_ || notify == 0) return std::string();
    requestReady_ = false;
    if(state_ == Details) return detailRequest(notify);
    if(state_ != Bindings) return std::string();
    std::ostringstream out;
    out << '@' << notify << " get :hwbind:" << bindSlot_ + 1 << ':' << bindPage_ << " get :modecount\n";
    return out.str();
}

void HwCacheBatch::nextSlot(unsigned from){
    for(unsigned i = from; i < 3; ++i)
        if(mask_ & (1u << i)){
            bindSlot_ = i;
            bindPage_ = 1;
            bindPages_ = 0;
            bindGen_ = 0;
            pageSeen_ = false;
            noCache_ = false;
            bindWords_.clear();
            requestReady_ = true;
            return;
        }
    state_ = Complete;
}

bool HwCacheBatch::acceptPage(const std::string& line){
    HwBinding::Page page;
    std::string err;
    if(!HwBinding::parsePage(line, page, err)){ fail("invalid page: " + err); return true; }
    if(page.mode != bindSlot_ + 1){ fail("page of another slot"); return true; }
    if(pageSeen_){ fail("duplicate page"); return true; }
    pageSeen_ = true;
    if(page.error){
        // No record for a slot the daemon has no cache of; anything else (range) is a request gone wrong
        if(page.why != "nocache" || bindPage_ != 1){ fail("page error " + page.why); return true; }
        noCache_ = true;
        return true;
    }
    if(page.page != bindPage_){ fail("page out of order"); return true; }
    if(bindPage_ == 1){
        bindPages_ = page.pages;
        bindGen_ = page.gen;
    } else if(page.pages != bindPages_ || page.gen != bindGen_){
        fail("the slot changed between pages"); return true;
    }
    bindWords_.insert(bindWords_.end(), page.words.begin(), page.words.end());
    return true;
}

bool HwCacheBatch::pageMarker(){
    // A marker with no page before it: the page was lost (a full notification pipe drops whole lines)
    if(!pageSeen_){ fail("lost page"); return true; }
    if(noCache_){ nextSlot(bindSlot_ + 1); return true; }
    if(bindPage_ < bindPages_){
        ++bindPage_;
        pageSeen_ = false;
        requestReady_ = true;
        return true;
    }
    Mode& mode = pending_.modes[bindSlot_];
    std::string err;
    if(!HwBinding::parseRecord(bindWords_, mode.record, err)){ fail("invalid record: " + err); return true; }
    mode.hasRecord = true;
    mode.gen = bindGen_;
    nextSlot(bindSlot_ + 1);
    return true;
}

std::string HwCacheBatch::identityRequest(unsigned notify) const{
    if(state_ != Identity || notify == 0) return std::string();
    std::ostringstream out;
    out << '@' << notify << " get :hwprofileid";
    for(unsigned i = 0; i < 3; ++i)
        if(mask_ & (1u << i)) out << " mode " << i + 1 << " get :hwid";
    out << " get :modecount\n";
    return out.str();
}

std::string HwCacheBatch::detailRequest(unsigned notify) const{
    if(state_ != Details || notify == 0) return std::string();
    std::ostringstream out;
    out << '@' << notify << " get :hwprofilename";
    for(unsigned i = 0; i < 3; ++i)
        if(mask_ & (1u << i)) out << " mode " << i + 1 << " get :hwname :hwrgb";
    out << " get :modecount\n";
    return out.str();
}

void HwCacheBatch::fail(const char* reason){
    state_ = Failed;
    error_ = reason;
    requestReady_ = false;
}

void HwCacheBatch::fail(const std::string& reason){
    fail(reason.c_str());
}

bool HwCacheBatch::accept(const std::string& line){
    if(state_ == Complete) return false;
    const auto w = words(line);
    if(w.empty()) return false;
    if(state_ == Failed){
        if(w[0] == "modecount") failureDrained_ = true;
        // (a "mode <m> hwbind" page too: every "mode" line is taken)
        // A bad response must not leak the rest of its batch into the legacy
        // incremental parser while we await the ordered terminal marker.
        return w[0] == "modecount" || w[0] == "hwprofileid" ||
               w[0] == "hwprofilename" || w[0] == "mode";
    }
    if(w[0] == "modecount"){
        failureDrained_ = true;
        unsigned count = 0;
        if(w.size() != 2 || !mode_count(w[1], count) ||
           (state_ != Identity && count != pending_.modeCount)){
            fail("invalid marker"); return true;
        }
        if(state_ == Bindings) return pageMarker();
        if(state_ == Identity){
            if(!profileIdSeen_ || modeIdsSeen_ != mask_){ fail("missing identity"); return true; }
            if(requireChangedIds_){
                // A partial refresh cannot safely be merged into a different
                // hardware profile: untouched modes belong to the old one.
                if(pending_.profileGuid != previous_.profileGuid){
                    fail("changed profile identity"); return true;
                }
                for(unsigned i = 0; i < 3; ++i)
                    if((mask_ & (1u << i)) && pending_.modes[i].guid == previous_.modes[i].guid &&
                       pending_.modes[i].revision == previous_.modes[i].revision){
                        fail("unchanged mode identity"); return true;
                    }
            }
            pending_.modeCount = count;
            state_ = Details;
            requestReady_ = true;
            failureDrained_ = false; // the second batch now has its own marker
        } else {
            if(!profileNameSeen_ || modeNamesSeen_ != mask_ ||
               (modeRgbSeen_ & rgbRequired_) != rgbRequired_){
                fail("missing details"); return true;
            }
            if(bindings_){
                state_ = Bindings;
                failureDrained_ = false; // every page has its own marker
                nextSlot(0);
            } else
                state_ = Complete;
        }
        return true;
    }
    if(state_ == Bindings){
        if(w.size() >= 3 && w[0] == "mode" && w[2] == "hwbind") return acceptPage(line);
        // A late identity or detail answer here is out of order
        if(w[0] == "hwprofileid" || w[0] == "hwprofilename" || w[0] == "mode"){ fail("unexpected line"); return true; }
        return false;
    }
    if(state_ == Identity && w[0] == "hwprofileid"){
        if(w.size() != 3 || profileIdSeen_ || !valid_guid(w[1]) || !valid_revision(w[2]))
            fail("invalid profile identity");
        else {
            pending_.profileGuid = w[1]; pending_.profileRevision = w[2]; profileIdSeen_ = true;
        }
        return true;
    }
    if(state_ == Details && w[0] == "hwprofilename"){
        if(w.size() != 2 || profileNameSeen_ || !valid_name(w[1])) fail("invalid profile name");
        else { pending_.profileName = w[1]; profileNameSeen_ = true; }
        return true;
    }
    if(w[0] != "mode" || w.size() < 3) return false;
    if(w[1].size() != 1 || w[1][0] < '1' || w[1][0] > '3') return false;
    const unsigned index = static_cast<unsigned>(w[1][0] - '1');
    const unsigned bit = 1u << index;
    if(!(mask_ & bit)) return false;
    if(state_ == Identity && w[2] == "hwid"){
        if(w.size() != 5 || (modeIdsSeen_ & bit) || !valid_guid(w[3]) || !valid_revision(w[4]))
            fail("invalid mode identity");
        else {
            pending_.modes[index].guid = w[3]; pending_.modes[index].revision = w[4];
            modeIdsSeen_ |= bit;
        }
        return true;
    }
    if(state_ == Details && w[2] == "hwname"){
        if(w.size() != 4 || (modeNamesSeen_ & bit) || !valid_name(w[3])) fail("invalid mode name");
        else { pending_.modes[index].name = w[3]; modeNamesSeen_ |= bit; }
        return true;
    }
    if(state_ == Details && w[2] == "hwrgb"){
        if(!valid_rgb(w) || (modeRgbSeen_ & bit)) fail("invalid mode rgb");
        else {
            pending_.modes[index].rgb = line.substr(line.find("hwrgb") + 6);
            pending_.modes[index].hasRgb = true;
            modeRgbSeen_ |= bit;
        }
        return true;
    }
    return false;
}
