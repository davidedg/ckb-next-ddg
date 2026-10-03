#include "k95psnapshot.h"

#include "hwsavepipe.h"
#include "k95pledmap.h"

#include <QByteArray>
#include <QSet>
#include <algorithm>
#include <set>

namespace {
int ledForName(const QString& name){
    for(size_t i = 0; i < K95PLedMap::count; ++i)
        if(name == QLatin1String(K95PLedMap::entries[i].name))
            return K95PLedMap::entries[i].led;
    return -1;
}

int ledForCell(int cell){
    if(cell >= 144) return cell;
    if(cell < 96) return 12 * (cell / 8) + cell % 8;
    const int col = cell / 8 - 12, within = cell % 8;
    return 12 * (col + 6 * (within / 4)) + 8 + within % 4;
}

std::string rgbHex(uint32_t rgb){
    static const char digits[] = "0123456789abcdef";
    std::string out(6, '0');
    for(int i = 5; i >= 0; --i){ out[size_t(i)] = digits[rgb & 15u]; rgb >>= 4; }
    return out;
}

bool addLine(K95PSnapshot& result, const std::string& line){
    // POSIX guarantees at least 512 bytes of PIPE_BUF. The actual descriptor
    // limit is checked again by HwSavePipe before a line is delivered.
    if(line.size() > 512 || line.empty() || line.back() != '\n') return false;
    result.prepare.push_back(line);
    return true;
}
}

bool makeK95PSnapshot(const KeyMap& map, const QColorMap& colours,
                      const QString& name, unsigned mode,
                      K95PSnapshot& out, std::string& error){
    error.clear();
    K95PSnapshot result;
    if(map.model() != KeyMap::K95P || mode < 1 || mode > 3){ error = "invalid K95P mode"; return false; }
    const QByteArray nameUtf8 = name.toUtf8();
    if(QString::fromUtf8(nameUtf8) != name){ error = "invalid UTF-8 name"; return false; }
    std::string encoded;
    if(!HwSavePipe::encode_name(std::string(nameUtf8.constData(), size_t(nameUtf8.size())), encoded)){
        error = "empty or invalid name"; return false;
    }
    const std::string prefix = "mode " + std::to_string(mode);
    if(!addLine(result, prefix + " name " + encoded + "\n") ||
       !addLine(result, prefix + " rgb 000000\n")){
        error = "command exceeds PIPE_BUF minimum"; return false;
    }

    QStringList names = map.keys();
    names.sort(Qt::CaseSensitive);
    std::string line = prefix + " rgb";
    unsigned grouped = 0;
    for(const QString& keyName : names){
        const Key key = map.key(keyName);
        if(!key.hasLed) continue;
        const uint32_t rgb = static_cast<uint32_t>(colours.value(keyName, 0)) & 0xffffffu;
        const int led = ledForName(keyName);
        if(led < 0 || led >= int(result.ledRgb.size())){
            if(rgb) result.invisibleKeys = true;
            continue;
        }
        const QByteArray ascii = keyName.toLatin1();
        if(QString::fromLatin1(ascii) != keyName || ascii.isEmpty() ||
           ascii.contains(' ') || ascii.contains(':') || ascii.contains('\n')){
            error = "invalid key name"; return false;
        }
        const std::string token = " " + std::string(ascii.constData(), size_t(ascii.size())) + ":" + rgbHex(rgb);
        if(grouped == 16 || line.size() + token.size() + 1 > 512){
            if(!addLine(result, line + "\n")){ error = "RGB command too long"; return false; }
            line = prefix + " rgb"; grouped = 0;
        }
        if(line.size() + token.size() + 1 > 512){ error = "RGB key command too long"; return false; }
        line += token;
        ++grouped;
        result.ledRgb[size_t(led)] = rgb;
    }
    if(grouped && !addLine(result, line + "\n")){ error = "RGB command too long"; return false; }

    bool canonical[256] = {};
    std::set<uint32_t> distinct;
    for(size_t i = 0; i < K95PLedMap::canonicalCount; ++i){
        const int led = ledForCell(K95PLedMap::canonicalCells[i]);
        if(led < 0 || led >= int(result.ledRgb.size())){ error = "invalid canonical LED"; return false; }
        canonical[led] = true;
        const uint32_t rgb = result.ledRgb[size_t(led)];
        if(rgb) distinct.insert(rgb);
    }
    for(size_t led = 0; led < result.ledRgb.size(); ++led)
        if(result.ledRgb[led] && !canonical[led] && led != 0x7d && led != 0x89 && led != 0x08)
            result.invisibleKeys = true;
    result.colours = unsigned(distinct.size());
    result.allBlack = distinct.empty();
    out = std::move(result);
    return true;
}
