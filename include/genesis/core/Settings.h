#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "AiBusHook.h"

namespace Genesis {

// Lightweight persistent key-value store backed by a plain text file.
// Keys use dot notation: "serial.port", "ui.theme", "window.width".
// Call Settings::instance().setPath(...).load() at startup.
class Settings {
public:
    static Settings& instance() {
        static Settings inst;
        return inst;
    }

    void setPath(const std::filesystem::path& path) { m_path = path; }
    const std::filesystem::path& path() const { return m_path; }

    // Setters
    void set(const std::string& key, std::string value) {
        m_data[key] = value;
        if (AiBusHook::emit) AiBusHook::emit(0, ("settings:" + key).c_str(), value.c_str());
    }
    void set(const std::string& key, int    value) { set(key, std::to_string(value)); }
    void set(const std::string& key, double value) { set(key, std::to_string(value)); }
    void set(const std::string& key, bool   value) { set(key, std::string(value ? "true" : "false")); }

    // Typed getters with defaults
    template<typename T>
    T get(const std::string& key, const T& def = T{}) const;

    bool has(const std::string& key) const { return m_data.count(key) > 0; }
    void remove(const std::string& key)    { m_data.erase(key); }
    void clear()                           { m_data.clear(); m_path.clear(); }

    void load() {
        if (m_path.empty() || !std::filesystem::exists(m_path)) return;
        std::ifstream f(m_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq != std::string::npos)
                m_data[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    void save() const {
        if (m_path.empty()) return;
        std::filesystem::create_directories(m_path.parent_path());
        std::ofstream f(m_path);
        for (const auto& [k, v] : m_data)
            f << k << '=' << v << '\n';
    }

private:
    Settings() = default;
    std::unordered_map<std::string, std::string> m_data;
    std::filesystem::path m_path;
};

template<> inline std::string Settings::get<std::string>(const std::string& k, const std::string& def) const {
    auto it = m_data.find(k); return it != m_data.end() ? it->second : def;
}
template<> inline int Settings::get<int>(const std::string& k, const int& def) const {
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}
template<> inline double Settings::get<double>(const std::string& k, const double& def) const {
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}
template<> inline bool Settings::get<bool>(const std::string& k, const bool& def) const {
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    return it->second == "true" || it->second == "1" || it->second == "yes";
}

} // namespace Genesis
