// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/JLocalWebServer.h>

#include <j/core/Log.h>
#include <j/io/JTcpSocket.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

inline namespace jf {

namespace {

constexpr int    kPollMs        = 200;     // how quickly stop() is noticed by the accept loop
constexpr int    kRecvTimeoutMs = 2000;    // a client that never finishes its request is dropped
constexpr size_t kMaxRequest    = 8192;    // a request line and headers; nothing here takes a body

void respond(JTcpSocket& s, int status, const char* reason, const std::string& type,
             const std::string& body, bool head) {
    std::ostringstream h;
    h << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-cache\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Connection: close\r\n\r\n";
    std::string unsent;                 // a browser that went away mid-answer: nothing to tell anyone
    if (s.write(h.str(), unsent) && !head) s.write(body, unsent);
}

std::string percentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1]))
                                             && std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

} // namespace

JLocalWebServer::JLocalWebServer() = default;
JLocalWebServer::~JLocalWebServer() { stop(); }

void JLocalWebServer::mount(const std::string& prefix, const std::string& dir) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(fs::path(dir), ec);
    m_mounts.emplace_back(prefix, (ec ? fs::path(dir) : canon).string());
}

std::string JLocalWebServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(m_server.port()) + "/" + path;
}

std::string JLocalWebServer::mimeType(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    static const std::pair<const char*, const char*> kTypes[] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },   { ".js", "text/javascript; charset=utf-8" },
        { ".json", "application/json" },         { ".svg", "image/svg+xml" },
        { ".png", "image/png" },                 { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },                 { ".webp", "image/webp" }, { ".ico", "image/x-icon" },
        { ".woff", "font/woff" },                { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" },
        { ".txt", "text/plain; charset=utf-8" }, { ".xml", "application/xml" }, { ".map", "application/json" },
    };
    for (const auto& [e, t] : kTypes) if (ext == e) return t;
    return "application/octet-stream";
}

std::string JLocalWebServer::resolve(const std::string& requestPath) const {
    if (requestPath.empty() || requestPath.front() != '/' || requestPath.find('\0') != std::string::npos)
        return {};
    const std::string rest = requestPath.substr(1);
    const size_t slash = rest.find('/');
    const std::string prefix = rest.substr(0, slash);
    const std::string inside = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    for (const auto& [mountPrefix, dir] : m_mounts) {
        if (mountPrefix != prefix) continue;
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(dir) / fs::path(inside), ec);
        if (ec) return {};
        // INSIDE THE MOUNT, OR NOTHING. Canonicalising resolves every ".." and symlink first, so what
        // is compared is where the path really goes, not how it was spelled.
        const fs::path rel = p.lexically_relative(fs::path(dir));
        if (rel.empty() || *rel.begin() == "..") return {};
        if (fs::is_directory(p, ec)) p /= "index.html";
        if (!fs::is_regular_file(p, ec)) return {};
        return p.string();
    }
    return {};
}

bool JLocalWebServer::start(std::string& error) {
    if (m_running.load()) return true;
    if (!m_server.listen(0, error)) return false;    // the system picks a free port
    m_running.store(true);
    m_thread = std::thread([this] { serve(); });
    JLOGC("io.web", JLogLevel::Info) << "serving local pages on " << url("");
    return true;
}

void JLocalWebServer::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();      // the accept loop polls, so it sees the flag promptly
    m_server.close();
}

void JLocalWebServer::serve() {
    while (m_running.load()) {
        JTcpSocket c;
        if (!m_server.accept(c, kPollMs, kRecvTimeoutMs)) continue;

        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < kMaxRequest) {
            const int n = c.read(buf, sizeof buf);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }

        std::istringstream line(req.substr(0, req.find("\r\n")));
        std::string method, target;
        line >> method >> target;
        const bool head = method == "HEAD";
        if (method != "GET" && !head) {
            respond(c, 405, "Method Not Allowed", "text/plain; charset=utf-8", "GET only\n", false);
        } else {
            target = target.substr(0, target.find_first_of("?#"));
            const std::string file = resolve(percentDecode(target));
            std::ifstream f(file, std::ios::binary);
            if (file.empty() || !f) {
                respond(c, 404, "Not Found", "text/plain; charset=utf-8", "Not found\n", head);
            } else {
                std::ostringstream body;
                body << f.rdbuf();
                respond(c, 200, "OK", mimeType(file), body.str(), head);
            }
        }
    }
}

} // inline namespace jf
