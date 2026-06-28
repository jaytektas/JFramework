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
#include <genesis/core/Variant.h>
#include <genesis/core/VariantJson.h>

namespace Genesis {

// ============================================================================
// Settings — thread-safe persistent key-value store of Variant values.
//
// Keys use dot notation: "serial.port", "ui.theme", "window.width".
// Values are stored as Variant, so types survive in memory and (with the JSON
// backend) on disk. The legacy INI backend flattens to strings on save and
// reloads as strings; typed getters coerce on read, so it stays compatible.
//
//   1. Global singleton:
//        Settings::instance().setPath("~/.config/app.json").loadJson();
//        Settings::instance().set("serial.port", "/dev/ttyUSB0");
//        Settings::instance().set("serial.baud", 115200);
//        auto port = Settings::instance().get<std::string>("serial.port");
//        Variant raw = Settings::instance().value("serial.baud");  // typed
//
//   2. Scoped instance:
//        Settings ws;
//        ws.setPath(workspaceDir / "workspace.json").loadJson();
// ============================================================================
class Settings {
public:
    // Fires on the main thread whenever a value is set, with the new Variant.
    Core::Signal<std::string, Variant> onChange;

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
    // One typed setter: any int/double/bool/string/const char* (and Variant
    // itself) implicitly constructs a Variant.

    void set(const std::string& key, Variant value) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_data[key] = value;
        }
        if (AiBusHook::emit) AiBusHook::emit(0, ("settings:" + key).c_str(), value.toString().c_str());
        MainThreadDispatcher::instance().post([this, key, value]() mutable {
            onChange.emit(key, value);
        });
    }

    // Set without firing onChange — use for bulk load/restore operations.
    void setQuiet(const std::string& key, Variant value) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data[key] = std::move(value);
    }

    // ---- Getters ------------------------------------------------------------

    // Typed, coercing getter with default.
    template<typename T>
    T get(const std::string& key, const T& def = T{}) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_data.find(key);
        if (it == m_data.end()) return def;
        return it->second.template value<T>(def);
    }

    // Raw Variant accessor (Null when absent unless a default is supplied).
    Variant value(const std::string& key, Variant def = {}) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_data.find(key);
        return it != m_data.end() ? it->second : def;
    }

    bool has(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_data.count(key) > 0;
    }

    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data.erase(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_data.clear();
    }

    std::vector<std::string> keys(const std::string& prefix = "") const {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<std::string> out;
        for (const auto& [k, v] : m_data)
            if (prefix.empty() || k.rfind(prefix, 0) == 0)
                out.push_back(k);
        return out;
    }

    // ---- Persistence: INI (string-flattened, legacy/compatible) -------------

    Settings& load() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_path.empty() || !std::filesystem::exists(m_path)) return *this;
        std::ifstream f(m_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq != std::string::npos)
                m_data[line.substr(0, eq)] = Variant(line.substr(eq + 1));
        }
        return *this;
    }

    void save() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::unordered_map<std::string, std::string> flat;
        for (const auto& [k, v] : m_data) flat[k] = v.toString();
        _writeFlat(m_path, flat);
    }

    void saveAsync(std::function<void(bool)> cb = nullptr) {
        std::unordered_map<std::string, std::string> snapshot;
        std::filesystem::path                        path;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (const auto& [k, v] : m_data) snapshot[k] = v.toString();
            path = m_path;
        }
        std::thread([snapshot = std::move(snapshot), path = std::move(path), cb]() {
            bool ok = _writeFlat(path, snapshot);
            if (cb) MainThreadDispatcher::instance().post([cb, ok]{ cb(ok); });
        }).detach();
    }

    // ---- Persistence: JSON (type-preserving) --------------------------------

    bool saveJson(int indent = 2) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        VariantMap m;
        m.reserve(m_data.size());
        for (const auto& [k, v] : m_data) m.emplace_back(k, v);
        Json j = toJson(Variant(std::move(m)));
        if (m_path.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(m_path.parent_path(), ec);
        if (ec) return false;
        std::ofstream f(m_path);
        if (!f) return false;
        f << j.dump(indent);
        return f.good();
    }

    Settings& loadJson() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_path.empty() || !std::filesystem::exists(m_path)) return *this;
        auto opt = Json::tryParseFile(m_path.string());
        if (!opt || !opt->isObject()) return *this;
        Variant root = fromJson(*opt);
        for (const auto& [k, v] : root.toMap()) m_data[k] = v;
        return *this;
    }

private:
    mutable std::mutex                        m_mutex;
    std::unordered_map<std::string, Variant>  m_data;
    std::filesystem::path                     m_path;

    static bool _writeFlat(const std::filesystem::path& path,
                           const std::unordered_map<std::string, std::string>& data) {
        if (path.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        std::ofstream f(path);
        if (!f) return false;
        for (const auto& [k, v] : data) f << k << '=' << v << '\n';
        return f.good();
    }
};

} // namespace Genesis
