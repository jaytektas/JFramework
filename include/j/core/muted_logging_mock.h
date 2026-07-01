#pragma once

#ifndef GENESIS_LOGGING_MOCK_DEFINED
#define GENESIS_LOGGING_MOCK_DEFINED

#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>

#include "Log.h"    // the real category logger the qC* macros route through

inline namespace jf { namespace Log {

struct JCategory { const char* name; };

inline constexpr JCategory Core     {"Core"};
inline constexpr JCategory Layout   {"Layout"};
inline constexpr JCategory Graphics {"Graphics"};
inline constexpr JCategory Vulkan   {"Vulkan"};
inline constexpr JCategory Widgets  {"Widgets"};
inline constexpr JCategory Platform {"Platform"};
inline constexpr JCategory Assets   {"Assets"};
inline constexpr JCategory AI       {"AI"};
inline constexpr JCategory JSignal   {"JSignal"};

class JFileLogger {
public:
    static JFileLogger& instance() {
        static JFileLogger inst;
        return inst;
    }

    void log(const std::string& level, const std::string& category, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        std::string formatted = "[" + level + "][" + category + "] " + message;
        
        // Console output
        if (level == "WARNING" || level == "CRITICAL") {
            std::cerr << formatted;
            std::cerr.flush();
        } else {
            std::cout << formatted;
            std::cout.flush();
        }
        
        // File output
        if (m_file.is_open()) {
            m_file << formatted;
            m_file.flush();
        }
    }

private:
    JFileLogger() {
        m_file.open("genesis.log", std::ios::out | std::ios::app);
    }
    ~JFileLogger() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    std::ofstream m_file;
    std::mutex m_mutex;
};

class JLogStream {
public:
    JLogStream(const std::string& level, const std::string& category)
        : m_level(level), m_category(category) {}
        
    ~JLogStream() {
        std::string s = m_stream.str();
        if (s.empty() || s.back() != '\n') {
            s += "\n";
        }
        JFileLogger::instance().log(m_level, m_category, s);
    }

    template<typename T>
    JLogStream& operator<<(const T& val) {
        m_stream << val;
        return *this;
    }
    
    JLogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        m_stream << manip;
        return *this;
    }

private:
    std::string m_level;
    std::string m_category;
    std::stringstream m_stream;
};

}} // namespace Log (in jf)

// The legacy qC* macros now route through the category logger (JLog), so framework logs gain the
// same per-category level control as everything else. Category name = the JCategory's .name.
#define qCInfo(cat)     JLOGC((cat).name, ::jf::JLogLevel::Info)
#define qCWarning(cat)  JLOGC((cat).name, ::jf::JLogLevel::Warn)
#define qCCritical(cat) JLOGC((cat).name, ::jf::JLogLevel::Error)
#define qCDebug(cat)    JLOGC((cat).name, ::jf::JLogLevel::Debug)

#endif // GENESIS_LOGGING_MOCK_DEFINED
