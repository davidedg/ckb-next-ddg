#ifndef HWLIGHTINGMODEL_H
#define HWLIGHTINGMODEL_H

#include <string>
#include <vector>

extern "C" {
#include "../daemon/cape_lightmodel.h"
}

// Value ownership of the shared typed lighting model. No Qt, XML or firmware file reader in the GUI.
namespace HwLighting {

class Model {
public:
    Model() = default;
    explicit Model(const cape_lightmodel& source);
    Model(const Model& source);
    Model(Model&& source) noexcept;
    Model& operator=(Model source);
    ~Model();
    void swap(Model& other) noexcept;
    const cape_lightmodel& data() const { return model; }
    cape_lightmodel& data() { return model; }
private:
    cape_lightmodel model{};
};

// Atomic decode: neither out nor offset changes on failure. Stops at the end of lighting, before the next record property.
bool same(const Model& a, const Model& b);

bool read(const std::vector<std::string>& words, size_t& offset, Model& out, std::string& error);
// Atomic emission. txn is empty for a record body, or the transaction ID for staging.
bool write(const Model& model, std::vector<std::string>& out, std::string& error, const std::string& txn = std::string());

} // namespace HwLighting

#endif
