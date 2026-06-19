#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include "muted_logging_mock.h"

namespace Genesis {

/**
 * @brief Global translation lookup — use via the tr() free function below.
 *
 * Loads JSON catalogs from the translations/ directory.
 * Keys are source-language strings (English).  Supports simple plural forms
 * via the "{n} singular|{n} plural" convention.
 *
 * Thread-safety: setLocale / loadCatalog must be called before any render
 * threads start; tr() is read-only and safe to call from any thread.
 */
class TranslationEngine {
public:
    static TranslationEngine& instance() {
        static TranslationEngine s;
        return s;
    }

    // ---- Configuration ----

    void setSearchPath(const std::string& dir) { m_dir = dir; }

    void setLocale(const std::string& locale) {
        if (locale == m_locale) return;
        m_locale = locale;
        m_strings.clear();
        _load(locale);
        qCInfo(Genesis::Log::Core) << "Locale set to: " << locale << "\n";
    }

    const std::string& locale() const { return m_locale; }

    // ---- Translation ----

    // Singular: tr("Open File")
    std::string tr(const std::string& key) const {
        auto it = m_strings.find(key);
        return (it != m_strings.end()) ? it->second : key;
    }

    // Plural: tr("{n} item|{n} items", count) → "5 items"
    std::string tr(const std::string& key, int n) const {
        auto it = m_strings.find(key);
        std::string tmpl = (it != m_strings.end()) ? it->second : key;
        // Pick singular or plural half
        auto pipe = tmpl.find('|');
        std::string form = (n == 1 || pipe == std::string::npos)
                         ? tmpl.substr(0, pipe)
                         : tmpl.substr(pipe + 1);
        // Replace {n} with the actual number
        std::string result;
        std::string ns = std::to_string(n);
        size_t pos = 0, found;
        while ((found = form.find("{n}", pos)) != std::string::npos) {
            result += form.substr(pos, found - pos) + ns;
            pos = found + 3;
        }
        result += form.substr(pos);
        return result;
    }

    // List available bundled locales (reads translation dir)
    std::vector<std::string> availableLocales() const {
        return {"en", "fr", "de", "ja"};
    }

private:
    TranslationEngine() {
        // Auto-detect from environment
        const char* lang = std::getenv("LANG");
        if (lang) {
            std::string l(lang);
            auto under = l.find('_');
            m_locale = (under != std::string::npos) ? l.substr(0, under) : l;
        }
        // Default search path next to the binary
        m_dir = "./translations";
    }

    void _load(const std::string& locale) {
        std::string path = m_dir + "/" + locale + ".json";
        std::ifstream f(path);
        if (!f.is_open()) {
            // Try language-only fallback (e.g. "fr" from "fr_FR")
            auto under = locale.find('_');
            if (under != std::string::npos) {
                std::string base = locale.substr(0, under);
                f.open(m_dir + "/" + base + ".json");
                if (!f.is_open()) { qCWarning(Genesis::Log::Core) << "No catalog for: " << locale << "\n"; return; }
            } else {
                qCWarning(Genesis::Log::Core) << "No catalog for: " << locale << "\n";
                return;
            }
        }
        // Minimal JSON parser: find "key": "value" pairs (no nesting needed)
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        size_t pos = 0;
        while (pos < src.size()) {
            auto k = _nextString(src, pos);
            if (k.empty()) break;
            auto colon = src.find(':', pos);
            if (colon == std::string::npos) break;
            pos = colon + 1;
            auto v = _nextString(src, pos);
            if (k != "locale") m_strings[k] = v;
        }
        qCInfo(Genesis::Log::Core) << "Loaded " << m_strings.size()
                                   << " strings for locale '" << locale << "'\n";
    }

    static std::string _nextString(const std::string& s, size_t& pos) {
        auto start = s.find('"', pos);
        if (start == std::string::npos) { pos = s.size(); return {}; }
        std::string result;
        size_t i = start + 1;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '\\' && i < s.size()) { result += s[i++]; continue; }
            if (c == '"') break;
            result += c;
        }
        pos = i;
        return result;
    }

    std::string m_locale{"en"};
    std::string m_dir;
    std::unordered_map<std::string, std::string> m_strings;
};

// ---- Free function — matches Qt's tr() ergonomics ----
inline std::string tr(const std::string& key) {
    return TranslationEngine::instance().tr(key);
}
inline std::string tr(const std::string& key, int n) {
    return TranslationEngine::instance().tr(key, n);
}

} // namespace Genesis
