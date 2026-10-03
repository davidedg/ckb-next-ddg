#ifndef K95PSNAPSHOT_H
#define K95PSNAPSHOT_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "colormap.h"

// Immutable picture used both for local warnings and for the commands sent
// before hwsavecheck. No GUI/device state is changed while this is built.
struct K95PSnapshot {
    std::vector<std::string> prepare;
    std::array<uint32_t, 256> ledRgb{{}};
    unsigned colours = 0;
    bool invisibleKeys = false;
    bool allBlack = true;
};

bool makeK95PSnapshot(const KeyMap& map, const QColorMap& colours,
                      const QString& name, unsigned mode,
                      K95PSnapshot& out, std::string& error);

#endif
