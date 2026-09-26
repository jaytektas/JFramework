// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/JLocalWebServer.h>

#include <j/core/Log.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using SocketHandle = SOCKET;
  static constexpr SocketHandle kNoSocket = INVALID_SOCKET;
  static void closeSocket(SocketHandle s) { ::closesocket(s); }
  static int  pollOne(SocketHandle s, int ms) { WSAPOLLFD p{ s, POLLRDNORM, 0 }; return WSAPoll(&p, 1, ms); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using SocketHandle = int;
  static constexpr SocketHandle kNoSocket = -1;
  static void closeSocket(SocketHandle s) { ::close(s); }
  static int  pollOne(SocketHandle s, int ms) { pollfd p{ s, POLLIN, 0 }; return ::poll(&p, 1, ms); }
#endif

namespace fs = std::filesystem;

inline namespace jf {

namespace {

constexpr int    kPollMs       = 200;      // how quickly stop() is noticed by the accept loop
constexpr int    kRecvTimeoutS = 2;        // a client that never finishes its request is dropped
constexpr size_t kMaxRequest   = 8192;     // a request line and headers; nothing here takes a body

void ensureSockets() {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); });
#endif
}

bool sendAll(SocketHandle s, const char* data, size_t len) {
    while (len > 0) {
        const auto n = ::send(s, data, static_cast<int>(len), 0);
        if (n <= 0) return false;
        data += n;
        len  -= static_cast<size_t>(n);
    }
    return true;
}

void respond(SocketHandle s, int status, const char* reason, const std::string& type,
             const std::string& body, bool head) {
    std::ostringstream h;
    h << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-cache\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Connection: close\r\n\r\n";
    const std::string header = h.str();
    if (sendAll(s, header.data(), header.size()) && !head) sendAll(s, body.data(), body.size());
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
    return "http://127.0.0.1:" + std::to_string(m_port) + "/" + path;
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
    ensureSockets();
    const SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) { error = "cannot create a socket"; return false; }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only: never reachable from another machine
    addr.sin_port        = 0;                        // the system picks a free port
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 16) != 0) {
        closeSocket(s);
        error = "cannot listen on 127.0.0.1";
        return false;
    }
    socklen_t len = sizeof addr;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port   = ntohs(addr.sin_port);
    m_listen = static_cast<long long>(s);
    m_running.store(true);
    m_thread = std::thread([this] { serve(); });
    JLOGC("io.web", JLogLevel::Info) << "serving local pages on " << url("");
    return true;
}

void JLocalWebServer::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();      // the accept loop polls, so it sees the flag promptly
    closeSocket(static_cast<SocketHandle>(m_listen));
    m_listen = -1;
}

void JLocalWebServer::serve() {
    const SocketHandle listener = static_cast<SocketHandle>(m_listen);
    while (m_running.load()) {
        if (pollOne(listener, kPollMs) <= 0) continue;
        const SocketHandle c = ::accept(listener, nullptr, nullptr);
        if (c == kNoSocket) continue;

#if defined(_WIN32)
        const DWORD tmo = kRecvTimeoutS * 1000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
#else
        const timeval tmo{ kRecvTimeoutS, 0 };
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#endif
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < kMaxRequest) {
            const auto n = ::recv(c, buf, sizeof buf, 0);
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
        closeSocket(c);
    }
}

} // inline namespace jf
