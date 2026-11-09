// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JUrl — scheme://host[:port][/path?query], split into the parts a client connects with.
//
// The schemes the framework speaks: http, https, ws, wss. The scheme decides the default port and
// whether the connection is secure. Anything else — no scheme, another scheme, no host, credentials
// in the authority (a security foot-gun nothing here needs: refused rather than silently dropped), a
// port out of range — parses as !valid, and is answered with an error, never a connection attempt.
// Header-only and dependency-free.

#include <cctype>
#include <cstdint>
#include <string>

inline namespace jf {

class JUrl {
public:
    std::string scheme;           // lower-case
    std::string host;
    std::string path{"/"};        // with the query; "/" when the url has none
    uint16_t    port{0};
    bool        secure{false};    // https, wss
    bool        valid{false};

    static JUrl parse(const std::string& url) {
        JUrl u;
        const size_t s = url.find("://");
        if (s == std::string::npos) return u;
        u.scheme = url.substr(0, s);
        for (char& c : u.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const uint16_t defaultPort = portFor(u.scheme);
        if (defaultPort == 0) return u;
        u.secure = u.scheme == "https" || u.scheme == "wss";
        u.port   = defaultPort;

        const size_t hostStart = s + 3;
        const size_t pathStart = url.find_first_of("/?", hostStart);
        std::string  authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos
                                                                                     : pathStart - hostStart);
        if (pathStart != std::string::npos) u.path = url[pathStart] == '/' ? url.substr(pathStart)
                                                                            : "/" + url.substr(pathStart);
        if (authority.empty() || authority.find('@') != std::string::npos) return u;

        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(']') == std::string::npos) {
            const std::string p = authority.substr(colon + 1);
            if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return u;
            const long v = std::stol(p);
            if (v <= 0 || v > 65535) return u;
            u.port    = static_cast<uint16_t>(v);
            authority = authority.substr(0, colon);
        }
        u.host  = authority;
        u.valid = !u.host.empty();
        return u;
    }

    // The scheme's own port: 80 / 443 for http(s) and ws(s); 0 for a scheme the framework does not speak.
    static uint16_t portFor(const std::string& scheme) {
        if (scheme == "http"  || scheme == "ws")  return 80;
        if (scheme == "https" || scheme == "wss") return 443;
        return 0;
    }

    // host, with ":port" when the port is not the scheme's own — what a Host header carries.
    std::string authority() const {
        return port == portFor(scheme) ? host : host + ":" + std::to_string(port);
    }

    // scheme://authority — what a path is resolved against.
    std::string origin() const { return scheme + "://" + authority(); }
};

} // inline namespace jf
