#include "hwlightingmodel.h"

#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace HwLighting {

Model::Model(const cape_lightmodel& source){
    const int rc = cape_lightmodel_copy(&source, &model);
    if(rc == CAPE_E_CAP) throw std::bad_alloc();
    if(rc != CAPE_OK) throw std::invalid_argument("invalid lighting model");
}

Model::Model(const Model& source) : Model(source.model) {}

Model::Model(Model&& source) noexcept { swap(source); }

Model& Model::operator=(Model source){ swap(source); return *this; }

Model::~Model(){ cape_lightmodel_free(&model); }

void Model::swap(Model& other) noexcept { std::swap(model, other.model); }

bool same(const Model& a, const Model& b){
    if(a.data().known != b.data().known) return false;
    if(!a.data().known) return true;
    cape_profile_profile x{}, y{};
    x.layers = a.data().layers; x.nlayers = a.data().count;
    y.layers = b.data().layers; y.nlayers = b.data().count;
    return cape_profile_equal(&x, &y) == 1;
}

bool read(const std::vector<std::string>& words, size_t& offset, Model& out, std::string& error){
    error.clear();
    std::unique_ptr<cape_lightmodel_reader, decltype(&cape_lightmodel_reader_free)> reader(
        cape_lightmodel_reader_new(), cape_lightmodel_reader_free);
    if(!reader){ error = "out of memory reading lighting"; return false; }
    size_t next = offset;
    while(next < words.size() && !cape_lightmodel_ready(reader.get())){
        // Embedded NUL must not truncate a std::string at the C boundary.
        if(words[next].find('\0') != std::string::npos){ error = "invalid lighting word"; return false; }
        const int rc = cape_lightmodel_read_word(reader.get(), words[next++].c_str());
        if(rc != CAPE_OK){ error = rc == CAPE_E_CAP ? "out of memory reading lighting" : "invalid lighting model"; return false; }
    }
    if(!cape_lightmodel_ready(reader.get())){ error = "incomplete lighting model"; return false; }
    Model result;
    if(cape_lightmodel_take(reader.get(), &result.data()) != CAPE_OK){ error = "incomplete lighting model"; return false; }
    out.swap(result);
    offset = next;
    return true;
}

namespace {
int append(void* context, const char* word){
    try { static_cast<std::vector<std::string>*>(context)->emplace_back(word); }
    catch(const std::bad_alloc&) { return CAPE_E_CAP; }
    return CAPE_OK;
}
}

bool write(const Model& model, std::vector<std::string>& out, std::string& error, const std::string& txn){
    error.clear();
    if(txn.find('\0') != std::string::npos){ error = "invalid lighting transaction"; return false; }
    std::vector<std::string> result;
    const int rc = cape_lightmodel_emit(&model.data(), txn.empty() ? nullptr : txn.c_str(), append, &result);
    if(rc != CAPE_OK){ error = rc == CAPE_E_CAP ? "out of memory writing lighting" : "invalid lighting model"; return false; }
    out.swap(result);
    return true;
}

} // namespace HwLighting
