#pragma once

// JLog — a category-based logger. Every message belongs to a named category (hierarchical by dotted
// name, e.g. "comms", "comms.telem"), and each category has its own runtime threshold. A message is
// emitted only when its level >= the category's threshold. Any category — or a whole subtree via a
// "prefix.*" pattern — can be retuned live, and every category is listable, so a control UI (or an AI
// agent over the bus) can see and drive logging granularly, right down to protocol bytes.
//
// Usage:
//   JLOGC("comms", JLogLevel::Info) << "connected " << sig;      // early-outs when the category is quiet
//   JLog::instance().setLevel("comms.*", JLogLevel::Trace);      // fire-hose one subtree
//   JLog::instance().hexDump(JLogLevel::Trace, "comms.telem", "[rx]", frame.data(), frame.size());
//
// Thread-safety: safe from any thread (state + output are mutex-guarded).

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <mutex>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <algorithm>

inline namespace jf {

// Ordered by severity. A category threshold of Info hides Debug/Trace; Off hides everything.
enum class JLogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

inline const char* jLogLevelName(JLogLevel l) {
    switch (l) {
        case JLogLevel::Trace: return "TRACE";
        case JLogLevel::Debug: return "DEBUG";
        case JLogLevel::Info:  return "INFO";
        case JLogLevel::Warn:  return "WARN";
        case JLogLevel::Error: return "ERROR";
        default:               return "OFF";
    }
}

class JLog {
public:
    static JLog& instance() { static JLog inst; return inst; }

    // The default threshold for categories with no explicit / prefix rule.
    void setGlobalLevel(JLogLevel l) { std::lock_guard<std::mutex> lk(m_mx); m_global = l; }
    JLogLevel globalLevel() const    { std::lock_guard<std::mutex> lk(m_mx); return m_global; }

    // Set a category's threshold. A trailing '*' (e.g. "comms.*" or "comms*") applies to the whole
    // subtree — every existing category under that prefix and any created later.
    void setLevel(const std::string& name, JLogLevel l) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!name.empty() && name.back() == '*') {
            const std::string prefix = name.substr(0, name.size() - 1);   // "comms." or "comms" or ""
            for (auto& [k, v] : m_levels)
                if (k.rfind(prefix, 0) == 0) v = l;
            m_prefixes.emplace_back(prefix, l);                           // future categories inherit it
        } else {
            m_levels[name] = l;
        }
    }

    // Threshold in force for a category (explicit rule, else the last matching prefix, else global).
    // Registers the category on first use so categories() can list it.
    JLogLevel levelOf(const std::string& name) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_levels.find(name);
        if (it != m_levels.end()) return it->second;
        JLogLevel lvl = m_global;
        for (const auto& [prefix, plvl] : m_prefixes)
            if (name.rfind(prefix, 0) == 0) lvl = plvl;                   // last match wins
        m_levels.emplace(name, lvl);
        return lvl;
    }

    bool enabled(const std::string& name, JLogLevel msg) { return msg >= levelOf(name); }

    // All categories seen so far, sorted — for a logging control panel or AI introspection.
    std::vector<std::string> categories() {
        std::lock_guard<std::mutex> lk(m_mx);
        std::vector<std::string> out;
        out.reserve(m_levels.size());
        for (const auto& [k, v] : m_levels) out.push_back(k);
        std::sort(out.begin(), out.end());
        return out;
    }

    void write(JLogLevel lvl, const std::string& cat, std::string msg) {
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        std::string line = std::string("[") + jLogLevelName(lvl) + "][" + cat + "] " + msg + "\n";
        std::lock_guard<std::mutex> lk(m_mx);
        (lvl >= JLogLevel::Warn ? std::cerr : std::cout) << line;
        if (m_file.is_open()) { m_file << line; m_file.flush(); }
    }

    // Protocol-byte dump: hex + ASCII, 16 bytes/row. Guarded — call freely; it no-ops when quiet.
    void hexDump(JLogLevel lvl, const std::string& cat, const std::string& label,
                 const uint8_t* data, size_t n) {
        if (!enabled(cat, lvl)) return;
        static const char* hx = "0123456789abcdef";
        std::ostringstream ss;
        ss << label << " (" << n << " bytes)";
        for (size_t i = 0; i < n; i += 16) {
            ss << "\n  ";
            std::string ascii;
            for (size_t j = 0; j < 16; ++j) {
                if (i + j < n) {
                    const uint8_t b = data[i + j];
                    ss << hx[b >> 4] << hx[b & 0xF] << ' ';
                    ascii += (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                } else {
                    ss << "   ";
                }
            }
            ss << ' ' << ascii;
        }
        write(lvl, cat, ss.str());
    }

    void setLogFile(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_file.is_open()) m_file.close();
        m_file.open(path, std::ios::out | std::ios::app);
    }

private:
    JLog() { m_file.open("genesis.log", std::ios::out | std::ios::app); }

    mutable std::mutex                          m_mx;
    JLogLevel                                   m_global{JLogLevel::Info};
    std::unordered_map<std::string, JLogLevel>  m_levels;    // explicit + memoized
    std::vector<std::pair<std::string, JLogLevel>> m_prefixes;
    std::ofstream                               m_file;
};

// A log line that early-outs: it only accumulates and writes when the category+level is enabled.
class JLogLine {
public:
    JLogLine(JLogLevel lvl, std::string cat) : m_lvl(lvl), m_cat(std::move(cat)) {}
    ~JLogLine() { JLog::instance().write(m_lvl, m_cat, m_ss.str()); }

    template <typename T> JLogLine& operator<<(const T& v) { m_ss << v; return *this; }
    JLogLine& operator<<(std::ostream& (*manip)(std::ostream&)) { m_ss << manip; return *this; }

private:
    JLogLevel          m_lvl;
    std::string        m_cat;
    std::ostringstream m_ss;
};

} // inline namespace jf

// Stream a message to a category at a level, evaluated (and streamed) only when enabled — the cheap
// early-out is the loop condition, so building the message is skipped entirely when the category is
// quiet. Usage: JLOGC("comms", jf::JLogLevel::Info) << "opened " << port;
#define JLOGC(cat, lvl) \
    for (bool _jl_on = ::jf::JLog::instance().enabled((cat), (lvl)); _jl_on; _jl_on = false) \
        ::jf::JLogLine((lvl), (cat))
