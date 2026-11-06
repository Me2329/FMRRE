#pragma once
#include <map>
#include <string>

namespace tw {

// Persistent key=value settings. Stored on /persist or /cache when available
// (so they survive a data wipe), otherwise in /tmp for the current session.
// The PIN is stored as SHA-256(pin + kPinSalt). Part of the settings format (settings from before the rename
// are taken over): never change it.
constexpr const char kPinSalt[] = ":twrpv2";

class Settings {
public:
    static Settings& get();
    void load(const std::string& path);
    void save();

    std::string str(const std::string& k, const std::string& def = "") const;
    int integer(const std::string& k, int def = 0) const;
    bool flag(const std::string& k, bool def = false) const;
    void set(const std::string& k, const std::string& v);
    void set(const std::string& k, int v) { set(k, std::to_string(v)); }
    void setFlag(const std::string& k, bool v) { set(k, v ? "1" : "0"); }
    const std::string& path() const { return path_; }

private:
    std::map<std::string, std::string> kv_;
    std::string path_;
};

}  // namespace tw
