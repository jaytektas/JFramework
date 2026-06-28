#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <thread>
#include <functional>
#include <genesis/core/Signal.h>
#include <genesis/core/MainThreadDispatcher.h>
#include <genesis/core/AiBusHook.h>

namespace Genesis {

// ============================================================================
// Settings — thread-safe persistent key-value store backed by a plain INI file.
//
// Keys use dot notation: "serial.port", "ui.theme", "window.width".
// Values are always stored as strings; typed getters convert on read.
//
// Two usage patterns:
//
//   1. Global singleton (app-wide config):
//        Settings::instance().setPath("~/.config/myapp.ini").load();
//        Settings::instance().set("serial.port", "/dev/ttyUSB0");
//        auto port = Settings::instance().get<std::string>("serial.port", "/dev/ttyUSB0");
//
//   2. Scoped instance (e.g. per-workspace):
//        Settings ws;
//        ws.setPath(workspaceDir / "workspace.ini").load();
// ============================================================================
class Settings {
public:
    // Fires on the main thread whenever a value is set.
    // Receives key and new value string.
    Core::Signal<std::string, std::string> onChange;

    Settings() = default;

    Settings(const Settings&)            = delete;
    Settings& operator=(const Settings&) = delete;
    Settings(Settings&&)                 = default;
    Settings& operator=(Settings&&)      = default;

    static Settings& instance() {
        static Settings inst;
        return inst;
    }

    // ---- Path ---------------------------------------------------------------

    Settings& setPath(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_path = path;
        return *this;
    }

    const std::filesystem::path& path() const { return m_path; }

    // ---- Setters ------------------------------------------------------------

    void set(const std::string& key, std::string value) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_data[key] = value;
        }
        if (AiBusHook::emit) AiBusHook::emit(0, ("settings:" + key).c_str(), value.c_str());
        MainThreadDispatcher::instance().post([this, key, value]() mutable {
            onChange.emit(key, value);
        });
    }
    void set(const std::string& key, int    v) { set(key, std::to_string(v)); }
    void set(const std::string& key, double v) { set(key, std::to_string(v)); }
    void set(const std::string& key, bool   v) { set(key, std::string(v ? "true" : "false")); }

    // Set without firing onChange — use for bulk load/restore operations.
    void setQuiet(const std::string& key, std::string value) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data[key] = std::move(value);
    }

    // ---- Typed getters with defaults ----------------------------------------

    template<typename T>
    T get(const std::string& key, const T& def = T{}) const;

    bool has(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_data.count(key) > 0;
    }

    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data.erase(key);
    }

    // Clear all values but keep the file path.
    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data.clear();
    }

    // Returns all keys that start with prefix (pass "" for all keys).
    std::vector<std::string> keys(const std::string& prefix = "") const {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<std::string> out;
        for (const auto& [k, v] : m_data)
            if (prefix.empty() || k.rfind(prefix, 0) == 0)
                out.push_back(k);
        return out;
    }

    // ---- Persistence --------------------------------------------------------

    Settings& load() {
        std::lock_guard<std::mutex> lk(m_mutex);
        _loadUnlocked();
        return *this;
    }

    void save() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        _saveUnlocked();
    }

    // Write to disk on a background thread; optional cb(success) on main thread.
    void saveAsync(std::function<void(bool)> cb = nullptr) {
        std::unordered_map<std::string, std::string> snapshot;
        std::filesystem::path                        path;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            snapshot = m_data;
            path     = m_path;
        }
        std::thread([snapshot = std::move(snapshot), path = std::move(path), cb]() {
            bool ok = _writeFile(path, snapshot);
            if (cb)
                MainThreadDispatcher::instance().post([cb, ok]{ cb(ok); });
        }).detach();
    }

private:
    mutable std::mutex                            m_mutex;
    std::unordered_map<std::string, std::string>  m_data;
    std::filesystem::path                         m_path;

    void _loadUnlocked() {
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

    void _saveUnlocked() const {
        _writeFile(m_path, m_data);
    }

    static bool _writeFile(const std::filesystem::path& path,
                           const std::unordered_map<std::string, std::string>& data) {
        if (path.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        std::ofstream f(path);
        if (!f) return false;
        for (const auto& [k, v] : data)
            f << k << '=' << v << '\n';
        return f.good();
    }
};

// ---- Typed getters ----------------------------------------------------------

template<> inline std::string Settings::get<std::string>(const std::string& k, const std::string& def) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_data.find(k);
    return it != m_data.end() ? it->second : def;
}
template<> inline int Settings::get<int>(const std::string& k, const int& def) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}
template<> inline double Settings::get<double>(const std::string& k, const double& def) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}
template<> inline bool Settings::get<bool>(const std::string& k, const bool& def) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_data.find(k);
    if (it == m_data.end()) return def;
    const auto& v = it->second;
    return v == "true" || v == "1" || v == "yes";
}

} // namespace Genesis
