// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/HttpClient.h>

#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#if JF_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif
#endif

inline namespace jf {

namespace {

// ---- URL ------------------------------------------------------------------
struct JUrl {
    std::string scheme, host, path;
    uint16_t    port{0};
    bool        secure{false};
    bool        valid{false};
};

JUrl parseUrl(const std::string& url) {
    JUrl u;
    const size_t s = url.find("://");
    if (s == std::string::npos) return u;
    u.scheme = url.substr(0, s);
    for (char& c : u.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (u.scheme != "http" && u.scheme != "https") return u;
    u.secure = (u.scheme == "https");
    u.port   = u.secure ? 443 : 80;

    const size_t hostStart = s + 3;
    const size_t pathStart = url.find('/', hostStart);
    std::string  authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos
                                                                                 : pathStart - hostStart);
    u.path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
    if (authority.empty()) return u;

    // Userinfo is not supported: credentials in a URL are a security foot-gun and
    // nothing here needs them. Reject rather than silently ignore them.
    if (authority.find('@') != std::string::npos) return u;

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        const std::string p = authority.substr(colon + 1);
        if (!p.empty() && p.find_first_not_of("0123456789") == std::string::npos) {
            const long v = std::stol(p);
            if (v <= 0 || v > 65535) return u;
            u.port    = static_cast<uint16_t>(v);
            authority = authority.substr(0, colon);
        }
    }
    u.host  = authority;
    u.valid = !u.host.empty();
    return u;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- Response head --------------------------------------------------------
// Splits "HTTP/1.1 200 OK\r\nHeader: v\r\n\r\n" into status + headers. Returns
// the offset of the body, or npos while the head is still incomplete.
size_t parseHead(const std::string& buf, int& status, std::vector<JHttpHeader>& headers) {
    const size_t end = buf.find("\r\n\r\n");
    if (end == std::string::npos) return std::string::npos;

    size_t line = buf.find("\r\n");
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
            std::string v = h.substr(colon + 1);
            const size_t b = v.find_first_not_of(" \t");
            headers.push_back({ h.substr(0, colon), b == std::string::npos ? std::string() : v.substr(b) });
        }
        if (stop == end) break;
        p = stop + 2;
    }
    return end + 4;
}

std::string headerOf(const std::vector<JHttpHeader>& hs, const std::string& name) {
    const std::string want = lower(name);
    for (const auto& h : hs) if (lower(h.name) == want) return h.value;
    return {};
}

// ---- Chunked transfer decoding --------------------------------------------
// Returns false when the encoding is malformed; `done` when the terminating
// zero-length chunk has been seen.
bool dechunk(const std::string& in, std::vector<uint8_t>& out, bool& done) {
    out.clear();
    done = false;
    size_t p = 0;
    while (p < in.size()) {
        const size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) return true;                 // size line not fully arrived
        const std::string sizeLine = in.substr(p, eol - p);
        const size_t semi = sizeLine.find(';');                    // chunk extensions: ignored, per RFC
        const std::string hex = semi == std::string::npos ? sizeLine : sizeLine.substr(0, semi);
        if (hex.empty() || hex.find_first_not_of("0123456789abcdefABCDEF \t") != std::string::npos) return false;
        const unsigned long n = std::strtoul(hex.c_str(), nullptr, 16);
        const size_t dataAt = eol + 2;
        if (n == 0) { done = true; return true; }
        if (dataAt + n > in.size()) return true;                   // body not fully arrived
        out.insert(out.end(), in.begin() + dataAt, in.begin() + dataAt + n);
        p = dataAt + n + 2;                                        // skip the chunk's trailing CRLF
    }
    return true;
}

// ---- Socket ---------------------------------------------------------------
// Everything below is one transfer's worth of connection state. It owns its fd
// and TLS session and closes both on the way out, so every early return in the
// transfer path is leak-free without a single explicit cleanup call.
class Connection {
public:
    ~Connection() { close(); }

    bool connect(const JUrl& url, int timeoutMs, std::string& err) {
#if defined(_WIN32)
        WSADATA wsa; static std::once_flag once;
        std::call_once(once, [&] { WSAStartup(MAKEWORD(2, 2), &wsa); });
#endif
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(url.port);
        const int rc = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 || !res) { err = "cannot resolve " + url.host; return false; }

        for (addrinfo* a = res; a; a = a->ai_next) {
            m_fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (m_fd < 0) continue;
            if (::connect(m_fd, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0) break;
            closeFd();
        }
        ::freeaddrinfo(res);
        if (m_fd < 0) { err = "cannot connect to " + url.host + ":" + port; return false; }

        int one = 1;
        ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        setSocketTimeout(timeoutMs);

        if (!url.secure) return true;
        return startTls(url.host, err);
    }

    bool write(const std::string& data, std::string& err) {
#if JF_HAVE_OPENSSL && !defined(_WIN32)
        if (m_ssl) {
            size_t sent = 0;
            while (sent < data.size()) {
                const int n = SSL_write(m_ssl, data.data() + sent, static_cast<int>(data.size() - sent));
                if (n <= 0) { err = "TLS write failed"; return false; }
                sent += static_cast<size_t>(n);
            }
            return true;
        }
#endif
        size_t sent = 0;
        while (sent < data.size()) {
            const auto n = ::send(m_fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) { err = "send failed"; return false; }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // >0 bytes read, 0 clean EOF, <0 error.
    int read(char* buf, int cap) {
#if JF_HAVE_OPENSSL && !defined(_WIN32)
        if (m_ssl) {
            const int n = SSL_read(m_ssl, buf, cap);
            if (n > 0) return n;
            const int e = SSL_get_error(m_ssl, n);
            return (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) ? 0 : -1;
        }
#endif
        const auto n = ::recv(m_fd, buf, static_cast<size_t>(cap), 0);
        return n < 0 ? -1 : static_cast<int>(n);
    }

    void close() {
#if JF_HAVE_OPENSSL && !defined(_WIN32)
        if (m_ssl) { SSL_shutdown(m_ssl); SSL_free(m_ssl); m_ssl = nullptr; }
        if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
#endif
        closeFd();
    }

private:
    void closeFd() {
        if (m_fd < 0) return;
#if defined(_WIN32)
        ::closesocket(m_fd);
#else
        ::close(m_fd);
#endif
        m_fd = -1;
    }

    void setSocketTimeout(int ms) {
#if defined(_WIN32)
        DWORD tv = static_cast<DWORD>(ms);
#else
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#endif
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    bool startTls(const std::string& host, std::string& err) {
#if JF_HAVE_OPENSSL && !defined(_WIN32)
        static std::once_flag once;
        std::call_once(once, [] { SSL_library_init(); SSL_load_error_strings(); });
        m_ctx = SSL_CTX_new(TLS_client_method());
        if (!m_ctx) { err = "cannot create TLS context"; return false; }
        // Verify the chain AND the hostname. A client that skips either is not a
        // secure client, it is a plaintext client with extra steps.
        SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);
        if (!SSL_CTX_set_default_verify_paths(m_ctx)) { err = "no system CA store"; return false; }
        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) { err = "cannot create TLS session"; return false; }
        SSL_set_fd(m_ssl, m_fd);
        SSL_set_tlsext_host_name(m_ssl, host.c_str());              // SNI
        X509_VERIFY_PARAM* vp = SSL_get0_param(m_ssl);
        X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!X509_VERIFY_PARAM_set1_host(vp, host.c_str(), 0)) { err = "cannot set verify host"; return false; }
        if (SSL_connect(m_ssl) != 1) {
            const long v = SSL_get_verify_result(m_ssl);
            err = v != X509_V_OK ? std::string("TLS certificate rejected: ") + X509_verify_cert_error_string(v)
                                 : "TLS handshake failed";
            return false;
        }
        return true;
#else
        (void)host;
        err = "this build has no TLS support (built without OpenSSL)";
        return false;
#endif
    }

    int m_fd{-1};
#if JF_HAVE_OPENSSL && !defined(_WIN32)
    SSL_CTX* m_ctx{nullptr};
    SSL*     m_ssl{nullptr};
#endif
};

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Windows: WinHTTP, an OS component — no OpenSSL, nothing to ship, and the system certificate store and
// proxy configuration come for free. Its own redirect following is turned OFF so that redirects behave
// identically on both platforms (the caller's loop handles them).
JHttpResponse exchange(const std::string& url, int timeoutMs,
                       const std::vector<JHttpHeader>& headers,
                       const std::function<void(int64_t, int64_t)>& onProgress) {
    JHttpResponse r;
    r.finalUrl = url;
    const JUrl u = parseUrl(url);
    if (!u.valid) { r.error = "malformed url: " + url; return r; }

    struct Handle {
        HINTERNET h{nullptr};
        ~Handle() { if (h) ::WinHttpCloseHandle(h); }
    } session, connect, request;

    session.h = ::WinHttpOpen(L"JFramework/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.error = "WinHttpOpen failed"; return r; }
    ::WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    connect.h = ::WinHttpConnect(session.h, widen(u.host).c_str(), u.port, 0);
    if (!connect.h) { r.error = "cannot connect to " + u.host; return r; }

    request.h = ::WinHttpOpenRequest(connect.h, L"GET", widen(u.path).c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) { r.error = "cannot create request"; return r; }
    DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
    ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));

    std::wstring extra;
    for (const auto& h : headers) extra += widen(h.name + ": " + h.value + "\r\n");

    if (!::WinHttpSendRequest(request.h, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                              extra.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "request failed (WinHTTP error " + std::to_string(::GetLastError()) + ")";
        return r;
    }

    DWORD status = 0, len = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);

    DWORD hlen = 0;
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                          nullptr, &hlen, WINHTTP_NO_HEADER_INDEX);
    if (hlen) {
        std::wstring raw(hlen / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                  raw.data(), &hlen, WINHTTP_NO_HEADER_INDEX)) {
            int ignored = 0;
            parseHead(narrow(raw) + "\r\n", ignored, r.headers);   // reuse the shared header parser
        }
    }
    const std::string cl = headerOf(r.headers, "Content-Length");
    const int64_t declared = cl.empty() ? 0 : std::strtoll(cl.c_str(), nullptr, 10);

    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request.h, &avail)) { r.error = "read failed"; return r; }
        if (avail == 0) break;
        const size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!::WinHttpReadData(request.h, r.body.data() + at, avail, &got)) { r.error = "read failed"; return r; }
        r.body.resize(at + got);
        if (onProgress) onProgress(static_cast<int64_t>(r.body.size()), declared);
        if (got == 0) break;
    }
    return r;
}

#else

// One request/response exchange, no redirect handling (that is the caller's).
JHttpResponse exchange(const std::string& url, int timeoutMs,
                       const std::vector<JHttpHeader>& headers,
                       const std::function<void(int64_t, int64_t)>& onProgress) {
    JHttpResponse r;
    r.finalUrl = url;

    const JUrl u = parseUrl(url);
    if (!u.valid) { r.error = "malformed url: " + url; return r; }
    if (u.secure && !JHttpClient::tlsAvailable()) {
        r.error = "https requested but this build has no TLS support";
        return r;
    }

    Connection conn;
    if (!conn.connect(u, timeoutMs, r.error)) return r;

    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
    if ((u.secure && u.port != 443) || (!u.secure && u.port != 80)) req += ":" + std::to_string(u.port);
    req += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    bool haveUA = false;
    for (const auto& h : headers) {
        if (lower(h.name) == "user-agent") haveUA = true;
        req += h.name + ": " + h.value + "\r\n";
    }
    if (!haveUA) req += "User-Agent: JFramework/1.0\r\n";
    req += "\r\n";
    if (!conn.write(req, r.error)) return r;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string raw;
    size_t   bodyAt      = std::string::npos;
    int64_t  declared    = 0;
    bool     chunked     = false;
    char     buf[16384];

    for (;;) {
        if (std::chrono::steady_clock::now() > deadline) {
            r.error = "timed out after " + std::to_string(timeoutMs) + " ms";
            return r;
        }
        const int n = conn.read(buf, static_cast<int>(sizeof(buf)));
        if (n < 0) { r.error = "read failed"; return r; }
        if (n == 0) break;                                        // server closed: everything we get
        raw.append(buf, static_cast<size_t>(n));

        if (bodyAt == std::string::npos) {
            bodyAt = parseHead(raw, r.status, r.headers);
            if (bodyAt != std::string::npos) {
                chunked  = lower(headerOf(r.headers, "Transfer-Encoding")).find("chunked") != std::string::npos;
                const std::string cl = headerOf(r.headers, "Content-Length");
                declared = cl.empty() ? 0 : std::strtoll(cl.c_str(), nullptr, 10);
            }
        }
        if (bodyAt == std::string::npos) continue;

        const int64_t got = static_cast<int64_t>(raw.size() - bodyAt);
        if (onProgress) onProgress(got, declared);
        if (!chunked && declared > 0 && got >= declared) break;   // whole body in hand: don't wait for FIN
        if (chunked) {
            bool done = false;
            std::vector<uint8_t> decoded;
            if (!dechunk(raw.substr(bodyAt), decoded, done)) { r.error = "malformed chunked response"; return r; }
            if (done) { r.body = std::move(decoded); return r; }
        }
    }

    if (bodyAt == std::string::npos) { r.error = "no HTTP response"; return r; }
    if (chunked) {
        bool done = false;
        if (!dechunk(raw.substr(bodyAt), r.body, done)) { r.error = "malformed chunked response"; return r; }
        return r;
    }
    r.body.assign(raw.begin() + static_cast<long>(bodyAt), raw.end());
    return r;
}

#endif // !_WIN32

// Resolve a Location against the URL it came from — servers send both absolute
// and relative forms, and a relative one against the wrong base fetches nothing.
std::string resolveRedirect(const std::string& base, const std::string& loc) {
    if (loc.find("://") != std::string::npos) return loc;
    const JUrl b = parseUrl(base);
    if (!b.valid) return loc;
    const std::string root = b.scheme + "://" + b.host +
                             ((b.secure && b.port != 443) || (!b.secure && b.port != 80)
                                  ? ":" + std::to_string(b.port) : std::string());
    if (!loc.empty() && loc[0] == '/') return root + loc;
    const size_t slash = b.path.rfind('/');
    return root + (slash == std::string::npos ? "/" : b.path.substr(0, slash + 1)) + loc;
}

} // namespace

// ---- JHttpResponse ---------------------------------------------------------
std::string JHttpResponse::header(const std::string& name) const { return headerOf(headers, name); }

// ---- JHttpClient -----------------------------------------------------------
struct JHttpClient::Impl {
    int                      timeoutMs{15000};
    int                      maxRedirects{5};
    std::vector<JHttpHeader> headers;
    // Shared with every in-flight thread: cleared by the destructor so a transfer
    // that outlives the client neither touches it nor fires its callback.
    std::shared_ptr<std::atomic<bool>> alive{ std::make_shared<std::atomic<bool>>(true) };
};

JHttpClient::JHttpClient() : m_impl(std::make_unique<Impl>()) {}

JHttpClient::~JHttpClient() { m_impl->alive->store(false); }

void JHttpClient::setTimeout(int ms)                  { m_impl->timeoutMs = ms > 0 ? ms : 1; }
int  JHttpClient::timeout() const                     { return m_impl->timeoutMs; }
void JHttpClient::setMaxRedirects(int n)              { m_impl->maxRedirects = n < 0 ? 0 : n; }

void JHttpClient::setHeader(const std::string& name, const std::string& value) {
    for (auto& h : m_impl->headers)
        if (lower(h.name) == lower(name)) { h.value = value; return; }
    m_impl->headers.push_back({ name, value });
}

bool JHttpClient::tlsAvailable() {
#if defined(_WIN32)
    return true;              // WinHTTP; TLS is the OS's, always present
#elif JF_HAVE_OPENSSL
    return true;
#else
    return false;             // built without OpenSSL: http:// works, https:// is refused
#endif
}

void JHttpClient::get(const std::string& url, std::function<void(const JHttpResponse&)> done) {
    const int  timeoutMs    = m_impl->timeoutMs;
    const int  maxRedirects = m_impl->maxRedirects;
    const auto headers      = m_impl->headers;
    auto       alive        = m_impl->alive;
    auto       progress     = &onProgress;

    std::thread([url, timeoutMs, maxRedirects, headers, alive, progress, done = std::move(done)] {
        JLOGC("http", jf::JLogLevel::Info) << "GET " << url;
        std::string  target = url;
        JHttpResponse r;
        for (int hop = 0; ; ++hop) {
            r = exchange(target, timeoutMs, headers,
                         [alive, progress](int64_t got, int64_t total) {
                             if (!alive->load()) return;
                             jf::JMainThreadDispatcher::instance().post([alive, progress, got, total] {
                                 if (alive->load()) progress->emit(got, total);
                             });
                         });
            const bool redirect = r.error.empty() && (r.status == 301 || r.status == 302 ||
                                                      r.status == 303 || r.status == 307 || r.status == 308);
            if (!redirect || hop >= maxRedirects) break;
            const std::string loc = r.header("Location");
            if (loc.empty()) break;
            target = resolveRedirect(target, loc);
            JLOGC("http", jf::JLogLevel::Info) << "  -> " << r.status << " redirect to " << target;
        }
        JLOGC("http", jf::JLogLevel::Info) << "  <- status " << r.status << ", " << r.body.size()
                                           << " byte(s)" << (r.error.empty() ? "" : ", error: " + r.error);
        if (!alive->load()) return;                      // client gone: the callback's captures may be too
        jf::JMainThreadDispatcher::instance().post([alive, done = std::move(done), r = std::move(r)] {
            if (alive->load() && done) done(r);
        });
    }).detach();
}

JHttpResponse JHttpClient::getSync(const std::string& url, int timeoutMs,
                                   const std::vector<JHttpHeader>& headers) {
    std::string  target = url;
    JHttpResponse r;
    for (int hop = 0; ; ++hop) {
        r = exchange(target, timeoutMs, headers, {});
        const bool redirect = r.error.empty() && (r.status == 301 || r.status == 302 ||
                                                  r.status == 303 || r.status == 307 || r.status == 308);
        if (!redirect || hop >= 5) break;
        const std::string loc = r.header("Location");
        if (loc.empty()) break;
        target = resolveRedirect(target, loc);
    }
    return r;
}

} // inline namespace jf
