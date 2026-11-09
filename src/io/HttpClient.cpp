// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/HttpClient.h>

#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/io/JHttpHead.h>
#include <j/io/JUrl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <j/io/JTcpSocket.h>
#endif

inline namespace jf {

namespace {

// ---- URL ------------------------------------------------------------------
// A url this client fetches: http or https. ws:// and the rest parse, but are not ours.
JUrl httpUrl(const std::string& url) {
    JUrl u = JUrl::parse(url);
    if (u.scheme != "http" && u.scheme != "https") u.valid = false;
    return u;
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

// A STALL IS NOT A SLOW DOWNLOAD. The socket's receive timeout was the whole transfer's timeout — ten
// minutes for an update download — so a connection that simply stopped sending (flaky wifi, a dropped
// route) sat behind a progress window that never moved for ten minutes. The whole transfer still gets
// its time; any one wait for data gets at most this.
constexpr int kStallMs = 30000;

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
                       const std::function<void(int64_t, int64_t)>& onProgress,
                       const std::string& method = "GET", const std::string& body = {}) {
    JHttpResponse r;
    r.finalUrl = url;
    const JUrl u = httpUrl(url);
    if (!u.valid) { r.error = "malformed url: " + url; return r; }

    struct Handle {
        HINTERNET h{nullptr};
        ~Handle() { if (h) ::WinHttpCloseHandle(h); }
    } session, connect, request;

    session.h = ::WinHttpOpen(L"JFramework/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.error = "WinHttpOpen failed"; return r; }
    ::WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, std::min(timeoutMs, kStallMs));   // receive: a stall

    connect.h = ::WinHttpConnect(session.h, widen(u.host).c_str(), u.port, 0);
    if (!connect.h) { r.error = "cannot connect to " + u.host; return r; }

    request.h = ::WinHttpOpenRequest(connect.h, widen(method).c_str(), widen(u.path).c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) { r.error = "cannot create request"; return r; }
    DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
    ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));

    std::wstring extra;
    for (const auto& h : headers) extra += widen(h.name + ": " + h.value + "\r\n");

    if (!::WinHttpSendRequest(request.h, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                              extra.empty() ? 0 : DWORD(-1),
                              body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                              DWORD(body.size()), DWORD(body.size()), 0) ||
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
            JHttpHead head;
            head.parse(narrow(raw) + "\r\n");                     // the shared header parser
            r.headers = std::move(head.headers);
        }
    }
    const std::string cl = r.header("Content-Length");
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
                       const std::function<void(int64_t, int64_t)>& onProgress,
                       const std::string& method = "GET", const std::string& body = {}) {
    JHttpResponse r;
    r.finalUrl = url;

    const JUrl u = httpUrl(url);
    if (!u.valid) { r.error = "malformed url: " + url; return r; }
    if (u.secure && !JHttpClient::tlsAvailable()) {
        r.error = "https requested but this build has no TLS support";
        return r;
    }

    const int stallMs = std::min(timeoutMs, kStallMs);
    JTcpSocket conn;
    if (!conn.connect(u.host, u.port, u.secure, stallMs, r.error)) return r;

    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.authority();
    req += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    bool haveUA = false;
    for (const auto& h : headers) {
        if (JHttpHeader::sameName(h.name, "User-Agent")) haveUA = true;
        req += h.name + ": " + h.value + "\r\n";
    }
    if (!haveUA) req += "User-Agent: JFramework/1.0\r\n";
    if (method != "GET") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;
    if (!conn.write(req, r.error)) return r;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string raw;
    JHttpHead head;
    size_t   bodyAt      = std::string::npos;
    int64_t  declared    = 0;
    bool     chunked     = false;
    char     buf[16384];

    for (;;) {
        if (std::chrono::steady_clock::now() > deadline) {
            r.error = "timed out after " + std::to_string(timeoutMs) + " ms";
            return r;
        }
        const int n = conn.read(buf, sizeof(buf));
        if (n == JTcpSocket::kStalled) {
            r.error = "the connection stalled (no data for " + std::to_string(stallMs / 1000) + " s)";
            return r;
        }
        if (n == JTcpSocket::kFailed) { r.error = "read failed"; return r; }
        if (n == JTcpSocket::kClosed) break;                      // server closed: everything we get
        raw.append(buf, static_cast<size_t>(n));

        if (bodyAt == std::string::npos) {
            bodyAt = head.parse(raw);
            if (bodyAt != std::string::npos) {
                r.status  = head.status;
                r.headers = head.headers;
                std::string te = r.header("Transfer-Encoding");
                for (char& c : te) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                chunked   = te.find("chunked") != std::string::npos;
                const std::string cl = r.header("Content-Length");
                declared  = cl.empty() ? 0 : std::strtoll(cl.c_str(), nullptr, 10);
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
    // CUT OFF PART-WAY is a failure, not a smaller file: the caller would otherwise get 24 MB of a 26 MB
    // download as the whole of it, and only a checksum further on stood between that and an install.
    if (declared > 0 && static_cast<int64_t>(raw.size() - bodyAt) < declared) {
        auto size = [](int64_t b) { return b < 10240 ? std::to_string(b) + " bytes" : std::to_string(b / 1024) + " KB"; };
        r.error = "the connection closed after " + size(static_cast<int64_t>(raw.size() - bodyAt)) + " of " + size(declared);
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
    const JUrl b = httpUrl(base);
    if (!b.valid) return loc;
    const std::string root = b.origin();
    if (!loc.empty() && loc[0] == '/') return root + loc;
    const size_t slash = b.path.rfind('/');
    return root + (slash == std::string::npos ? "/" : b.path.substr(0, slash + 1)) + loc;
}

} // namespace

// ---- JHttpResponse ---------------------------------------------------------
std::string JHttpResponse::header(const std::string& name) const { return JHttpHeader::find(headers, name); }

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
        if (JHttpHeader::sameName(h.name, name)) { h.value = value; return; }
    m_impl->headers.push_back({ name, value });
}

bool JHttpClient::tlsAvailable() {
#if defined(_WIN32)
    return true;              // WinHTTP; TLS is the OS's, always present
#else
    return JTcpSocket::tlsAvailable();   // OpenSSL; without it http:// works and https:// is refused
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

JHttpResponse JHttpClient::postSync(const std::string& url, const std::string& body, const std::string& contentType,
                                    int timeoutMs, std::vector<JHttpHeader> headers) {
    headers.push_back({ "Content-Type", contentType });
    return exchange(url, timeoutMs, headers, {}, "POST", body);
}

} // inline namespace jf
