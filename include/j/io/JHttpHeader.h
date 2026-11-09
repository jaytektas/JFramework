// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JHttpHeader — one header of an HTTP request or response, and the case-insensitive lookup every
// reader of headers needs (RFC 9110: field names are case-insensitive). Header-only.

#include <cctype>
#include <string>
#include <vector>

inline namespace jf {

struct JHttpHeader {
    std::string name;
    std::string value;

    // Do two field names name the same field?
    static bool sameName(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // The value of the field `name` among `headers`; "" when absent.
    static std::string find(const std::vector<JHttpHeader>& headers, const std::string& name) {
        for (const auto& h : headers) if (sameName(h.name, name)) return h.value;
        return {};
    }
};

} // inline namespace jf
