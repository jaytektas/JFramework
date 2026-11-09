// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JBase64 — base64 encoding (RFC 4648, standard alphabet, '=' padded), as HTTP headers carry binary
// values: the WebSocket handshake's Sec-WebSocket-Key and Sec-WebSocket-Accept. Header-only.

#include <cstdint>
#include <string>

inline namespace jf {

class JBase64 {
public:
    static std::string encode(const uint8_t* data, size_t len) {
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 3 <= len; i += 3) {
            const uint32_t v = uint32_t(data[i]) << 16 | uint32_t(data[i + 1]) << 8 | data[i + 2];
            out += alphabet[(v >> 18) & 63];
            out += alphabet[(v >> 12) & 63];
            out += alphabet[(v >> 6) & 63];
            out += alphabet[v & 63];
        }
        if (const size_t rest = len - i; rest > 0) {
            const uint32_t v = uint32_t(data[i]) << 16 | (rest == 2 ? uint32_t(data[i + 1]) << 8 : 0u);
            out += alphabet[(v >> 18) & 63];
            out += alphabet[(v >> 12) & 63];
            out += rest == 2 ? alphabet[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }
    static std::string encode(const std::string& s) { return encode(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
};

} // inline namespace jf
