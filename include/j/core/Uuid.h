#pragma once

// jf::makeUuid — a random RFC-4122 version-4 UUID as a lowercase, hyphenated string with no braces,
// e.g. "f47ac10b-58cc-4372-a567-0e02b2c3d479". The framework's canonical identity primitive: give every
// control/widget a stable unique id. Self-seeded per thread from std::random_device.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

inline namespace jf {

inline std::string makeUuid() {
    static thread_local std::mt19937_64 rng{ std::random_device{}() };
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4 (nibble in time_hi)
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 10xx (top of clock_seq)
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFFu),
                  static_cast<unsigned>(a & 0xFFFFu),
                  static_cast<unsigned>((b >> 48) & 0xFFFFu),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

} // inline namespace jf
