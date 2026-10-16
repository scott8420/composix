// core::load_settings / save_settings (s052).
#include "core/Settings.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace composix::core {

AppSettings load_settings(const std::string& file) {
    AppSettings out;
    std::ifstream f(file);
    if (!f) return out;
    try {
        nlohmann::json j;
        f >> j;
        if (!j.is_object()) return out;
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

bool save_settings(const std::string& file, const AppSettings& s) {
    try {
        fs::create_directories(fs::path(file).parent_path());
    } catch (const std::exception&) {
        return false;
    }
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : s) j[k] = v;
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace composix::core
