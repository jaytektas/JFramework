// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JSha1 — SHA-1 (FIPS 180-4). For protocols that name it, and nothing that needs it to resist an
// attacker: the WebSocket handshake (RFC 6455) proves a server understood the upgrade by hashing the
// client's key with SHA-1. SHA-1 is broken for collision resistance — never use it to check that
// content is what it claims to be; JSha256 is for that. Header-only and dependency-free, so it is the
// same code with or without OpenSSL.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

inline namespace jf {

class JSha1 {
public:
    using Digest = std::array<uint8_t, 20>;

    static Digest digest(const uint8_t* data, size_t len) {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

        auto block = [&](const uint8_t* p) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = uint32_t(p[4*i]) << 24 | uint32_t(p[4*i+1]) << 16 | uint32_t(p[4*i+2]) << 8 | p[4*i+3];
            for (int i = 16; i < 80; ++i) w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
                const uint32_t t = rotl(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rotl(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        };

        size_t i = 0;
        for (; i + 64 <= len; i += 64) block(data + i);
        uint8_t tail[128] = {};
        const size_t rest = len - i;
        std::memcpy(tail, data + i, rest);
        tail[rest] = 0x80;
        const size_t tailLen = rest + 9 <= 64 ? 64 : 128;
        const uint64_t bits = uint64_t(len) * 8;
        for (int k = 0; k < 8; ++k) tail[tailLen - 1 - k] = uint8_t(bits >> (8 * k));
        block(tail);
        if (tailLen == 128) block(tail + 64);

        Digest out{};
        for (int j = 0; j < 5; ++j)
            for (int k = 0; k < 4; ++k) out[size_t(4 * j + k)] = uint8_t(h[j] >> (24 - 8 * k));
        return out;
    }
    static Digest digest(const std::string& s) { return digest(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

    // Lower-case hex digest.
    static std::string hex(const std::string& s) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (uint8_t b : digest(s)) { out += digits[b >> 4]; out += digits[b & 0xF]; }
        return out;
    }
};

} // inline namespace jf
