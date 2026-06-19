#pragma once
// Single definition point for all Genesis logging macros and category tokens.
// Every Genesis header includes this instead of defining its own mock structs.

#ifndef GENESIS_LOGGING_MOCK_DEFINED
#define GENESIS_LOGGING_MOCK_DEFINED

#include <iostream>

// Macro API matches the Qt qC* logging surface so headers compile unchanged
// whether a real logging backend is wired in or not.
#define qCInfo(cat)     (std::cout  << "[INFO]["     << (cat).name << "] ")
#define qCWarning(cat)  (std::cerr  << "[WARNING]["  << (cat).name << "] ")
#define qCCritical(cat) (std::cerr  << "[CRITICAL][" << (cat).name << "] ")
#define qCDebug(cat)    (std::cout  << "[DEBUG]["    << (cat).name << "] ")

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

} // namespace Genesis::Log

#endif // GENESIS_LOGGING_MOCK_DEFINED
