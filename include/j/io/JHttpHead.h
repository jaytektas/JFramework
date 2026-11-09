// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JHttpHead — the head of an HTTP/1.1 response: the status line and the headers, up to the blank
// line. What JHttpClient reads before a body and what JWebSocket reads to see its upgrade accepted.
// Header-only.

#include <j/io/JHttpHeader.h>

#include <cstdlib>
#include <string>
#include <vector>

inline namespace jf {

class JHttpHead {
public:
    int                      status{0};
    std::vector<JHttpHeader> headers;

    // Parse "HTTP/1.1 200 OK\r\nName: value\r\n...\r\n\r\n" from the front of `buf`. Returns the offset
    // of whatever follows the head (the body), or npos while the head has not fully arrived.
    size_t parse(const std::string& buf) {
        const size_t end = buf.find("\r\n\r\n");
        if (end == std::string::npos) return std::string::npos;

        const size_t line = buf.find("\r\n");
        const std::string statusLine = buf.substr(0, line);
        const size_t sp1 = statusLine.find(' ');
        if (sp1 == std::string::npos) return std::string::npos;
        status = std::atoi(statusLine.c_str() + sp1 + 1);

        headers.clear();
        for (size_t p = line + 2; p < end; ) {
            const size_t e = buf.find("\r\n", p);
            const size_t stop = (e == std::string::npos || e > end) ? end : e;
            const std::string h = buf.substr(p, stop - p);
            const size_t colon = h.find(':');
            if (colon != std::string::npos) {
                const std::string v = h.substr(colon + 1);
                const size_t b = v.find_first_not_of(" \t");
                headers.push_back({ h.substr(0, colon), b == std::string::npos ? std::string() : v.substr(b) });
            }
            if (stop == end) break;
            p = stop + 2;
        }
        return end + 4;
    }

    // Case-insensitive; "" when absent.
    std::string header(const std::string& name) const { return JHttpHeader::find(headers, name); }
};

} // inline namespace jf
