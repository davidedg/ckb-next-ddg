#include "hwpickermap.h"
#include "k95pkeytable.h"

#include <algorithm>
#include <cmath>

namespace {
// KeyMap keeps its size to itself (as KeyMapDebug)
struct SizedMap : KeyMap {
    using KeyMap::KeyMap;
    void setSize(short w, short h){ keyWidth = w; keyHeight = h; }
};
} // namespace

namespace HwPickerMap {

KeyMap k95p(const KeyMap& device){
    SizedMap m(KeyMap::K95P, KeyMap::US);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for(const QString& name : m.keys()){
        Key& k = m[name];
        const int i = K95PKeyTable::indexOf(name.toStdString());
        if(!k.hasScan || i < 0 || unsigned(i) >= K95PKeyTable::KEYS || !(K95PKeyTable::icueKeyboard(unsigned(i)) || K95PKeyTable::icueMedia(unsigned(i)))){
            k.hasScan = false;
            k.hasLed = false;
            continue;
        }
        if(device.contains(name) && device.key(name)._friendlyName)
            k._friendlyName = device.key(name)._friendlyName;
        x0 = std::min(x0, k.x - k.width / 2.f);
        x1 = std::max(x1, k.x + k.width / 2.f);
        y0 = std::min(y0, k.y - k.height / 2.f);
        y1 = std::max(y1, k.y + k.height / 2.f);
    }
    if(!device.contains("bslash") && device.contains("hash") && device.key("hash")._friendlyName)
        m["bslash"]._friendlyName = device.key("hash")._friendlyName;
    const short dx = short(std::floor(x0)), dy = short(std::floor(y0));
    for(const QString& name : m.keys()){
        Key& k = m[name];
        k.x = short(k.x - dx);
        k.y = short(k.y - dy);
    }
    m.setSize(short(std::ceil(x1 - dx)), short(std::ceil(y1 - dy)));
    return m;
}

} // namespace HwPickerMap
