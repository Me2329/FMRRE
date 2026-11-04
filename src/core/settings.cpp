#include "core/settings.h"
#include "core/log.h"
#include "core/util.h"

namespace tw {

Settings& Settings::get() {
    static Settings s;
    return s;
}

void Settings::load(const std::string& path) {
    path_ = path;
    std::string data;
    if (!util::readFile(path, data)) {
        LOGI("settings: no config at %s, using defaults", path.c_str());
        return;
    }
    for (auto& line : util::split(data, '\n')) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        kv_[util::trim(line.substr(0, eq))] = util::trim(line.substr(eq + 1));
    }
    LOGI("settings: loaded %zu keys from %s", kv_.size(), path.c_str());
}

void Settings::save() {
    if (path_.empty()) return;
    util::mkdirs(util::dirname(path_));
    std::string out = "# FMRRE settings\n";
    for (auto& [k, v] : kv_) out += k + "=" + v + "\n";
    if (!util::writeFile(path_, out)) LOGW("settings: failed to save %s", path_.c_str());
}

std::string Settings::str(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
}

int Settings::integer(const std::string& k, int def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

bool Settings::flag(const std::string& k, bool def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : (it->second == "1" || it->second == "true");
}

void Settings::set(const std::string& k, const std::string& v) { kv_[k] = v; }

}  // namespace tw
