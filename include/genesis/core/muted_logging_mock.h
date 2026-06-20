#pragma once

#ifndef GENESIS_LOGGING_MOCK_DEFINED
#define GENESIS_LOGGING_MOCK_DEFINED

#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>

namespace Genesis::Log {

struct Category { const char* name; };

inline constexpr Category Core     {"Core"};
inline constexpr Category Layout   {"Layout"};
inline constexpr Category Graphics {"Graphics"};
inline constexpr Category Vulkan   {"Vulkan"};
inline constexpr Category Widgets  {"Widgets"};
inline constexpr Category Platform {"Platform"};
inline constexpr Category Assets   {"Assets"};
inline constexpr Category AI       {"AI"};
inline constexpr Category Signal   {"Signal"};

class FileLogger {
public:
    static FileLogger& instance() {
        static FileLogger inst;
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
    FileLogger() {
        m_file.open("genesis.log", std::ios::out | std::ios::app);
    }
    ~FileLogger() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    std::ofstream m_file;
    std::mutex m_mutex;
};

class LogStream {
public:
    LogStream(const std::string& level, const std::string& category)
        : m_level(level), m_category(category) {}
        
    ~LogStream() {
        std::string s = m_stream.str();
        if (s.empty() || s.back() != '\n') {
            s += "\n";
        }
        FileLogger::instance().log(m_level, m_category, s);
    }

    template<typename T>
    LogStream& operator<<(const T& val) {
        m_stream << val;
        return *this;
    }
    
    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        m_stream << manip;
        return *this;
    }

private:
    std::string m_level;
    std::string m_category;
    std::stringstream m_stream;
};

} // namespace Genesis::Log

#define qCInfo(cat)     (Genesis::Log::LogStream("INFO",     (cat).name))
#define qCWarning(cat)  (Genesis::Log::LogStream("WARNING",  (cat).name))
#define qCCritical(cat) (Genesis::Log::LogStream("CRITICAL", (cat).name))
#define qCDebug(cat)    (Genesis::Log::LogStream("DEBUG",    (cat).name))

#endif // GENESIS_LOGGING_MOCK_DEFINED
