// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/JTcpSocket.h>

#include "JSocketPlatform.h"

#include <algorithm>
#include <limits>
#include <mutex>

#if JF_HAVE_OPENSSL && !defined(_WIN32)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#define JF_TCP_TLS 1
#else
#define JF_TCP_TLS 0
#endif

inline namespace jf {

namespace {

constexpr size_t kMaxIo = static_cast<size_t>(std::numeric_limits<int>::max());   // one call's byte count

#if !JF_TCP_TLS
#if defined(_WIN32)
constexpr const char* kNoTls = "TLS over a raw TCP connection is not available on Windows";
#else
constexpr const char* kNoTls = "this build has no TLS support (built without OpenSSL)";
#endif
#endif

} // namespace

// Everything one connection owns. It closes its handle and its TLS session on the way out, so every
// early return on the connect path is leak-free without an explicit cleanup call.
struct JTcpSocket::Impl {
    JSocketPlatform::Handle fd{JSocketPlatform::kNone};
    int                     timeoutMs{0};
#if JF_TCP_TLS
    SSL_CTX* ctx{nullptr};
    SSL*     ssl{nullptr};
#endif

    ~Impl() { close(); }

    void close() {
#if JF_TCP_TLS
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
#endif
        JSocketPlatform::closeHandle(fd);
        fd = JSocketPlatform::kNone;
    }

    bool secure() const {
#if JF_TCP_TLS
        return ssl != nullptr;
#else
        return false;
#endif
    }

    // A non-blocking connect, so that an address that never answers costs `waitMs`, not the
    // system's own connect timeout (minutes).
    bool connectOne(const addrinfo* a, int waitMs) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == JSocketPlatform::kNone) return false;
        JSocketPlatform::setNonBlocking(fd, true);
        bool ok = ::connect(fd, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) == 0;
        if (!ok && JSocketPlatform::lastConnectInProgress())
            ok = JSocketPlatform::poll(fd, true, waitMs) > 0 && JSocketPlatform::pendingError(fd) == 0;
        if (!ok) { JSocketPlatform::closeHandle(fd); fd = JSocketPlatform::kNone; return false; }
        JSocketPlatform::setNonBlocking(fd, false);
        return true;
    }

#if JF_TCP_TLS
    bool startTls(const std::string& host, std::string& err) {
        static std::once_flag once;
        std::call_once(once, [] { SSL_library_init(); SSL_load_error_strings(); });
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { err = "cannot create TLS context"; return false; }
        // Verify the chain AND the hostname.
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (!SSL_CTX_set_default_verify_paths(ctx)) { err = "no system CA store"; return false; }
        ssl = SSL_new(ctx);
        if (!ssl) { err = "cannot create TLS session"; return false; }
        SSL_set_fd(ssl, static_cast<int>(fd));
        SSL_set_tlsext_host_name(ssl, host.c_str());              // SNI
        X509_VERIFY_PARAM* vp = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!X509_VERIFY_PARAM_set1_host(vp, host.c_str(), 0)) { err = "cannot set verify host"; return false; }
        if (SSL_connect(ssl) != 1) {
            const long v = SSL_get_verify_result(ssl);
            err = v != X509_V_OK ? std::string("TLS certificate rejected: ") + X509_verify_cert_error_string(v)
                                 : "TLS handshake failed";
            return false;
        }
        return true;
    }
#endif
};

JTcpSocket::JTcpSocket() : m_impl(std::make_unique<Impl>()) {}
JTcpSocket::~JTcpSocket() = default;

bool JTcpSocket::tlsAvailable() { return JF_TCP_TLS != 0; }

bool JTcpSocket::isOpen() const   { return m_impl->fd != JSocketPlatform::kNone; }
bool JTcpSocket::isSecure() const { return m_impl->secure(); }
int  JTcpSocket::timeout() const  { return m_impl->timeoutMs; }

void JTcpSocket::setTimeout(int ms) {
    m_impl->timeoutMs = std::max(ms, 1);
    if (isOpen()) JSocketPlatform::setTimeouts(m_impl->fd, m_impl->timeoutMs);
}

void JTcpSocket::close() { m_impl->close(); }

void JTcpSocket::adopt(long long handle, int timeoutMs) {
    m_impl->close();
    m_impl->fd = JSocketPlatform::fromOpaque(handle);
    JSocketPlatform::setNoDelay(m_impl->fd);
    setTimeout(timeoutMs);
}

bool JTcpSocket::connect(const std::string& host, uint16_t port, bool tls, int timeoutMs, std::string& error) {
    m_impl->close();
#if !JF_TCP_TLS
    // Refused before a byte is sent: there is no plaintext fallback to fall into.
    if (tls) { error = kNoTls; return false; }
#endif
    JSocketPlatform::startup();

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
        error = "cannot resolve " + host;
        return false;
    }
    bool connected = false;
    for (const addrinfo* a = res; a && !connected; a = a->ai_next) connected = m_impl->connectOne(a, timeoutMs);
    ::freeaddrinfo(res);
    if (!connected) { error = "cannot connect to " + host + ":" + service; return false; }

    JSocketPlatform::setNoDelay(m_impl->fd);
    setTimeout(timeoutMs);
#if JF_TCP_TLS
    if (tls && !m_impl->startTls(host, error)) { m_impl->close(); return false; }
#endif
    return true;
}

bool JTcpSocket::write(const void* data, size_t len, std::string& error) {
    if (!isOpen()) { error = "not connected"; return false; }
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
#if JF_TCP_TLS
    if (m_impl->ssl) {
        while (sent < len) {
            const int n = SSL_write(m_impl->ssl, p + sent, static_cast<int>(std::min(len - sent, kMaxIo)));
            if (n <= 0) { error = "TLS write failed"; return false; }
            sent += static_cast<size_t>(n);
        }
        return true;
    }
#endif
    while (sent < len) {
        const long long n = JSocketPlatform::sendSome(m_impl->fd, p + sent, len - sent);
        if (n <= 0) {
            error = JSocketPlatform::lastFailureWasTimeout() ? "the peer stopped accepting data" : "send failed";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

int JTcpSocket::read(void* buf, size_t cap) {
    if (!isOpen()) return kFailed;
    const size_t want = std::min(cap, kMaxIo);
#if JF_TCP_TLS
    if (m_impl->ssl) {
        const int n = SSL_read(m_impl->ssl, buf, static_cast<int>(want));
        if (n > 0) return n;
        const int e = SSL_get_error(m_impl->ssl, n);
        // The receive timeout surfaces as a SYSCALL error with EAGAIN — which, read as EOF, turned a
        // stalled download into a SHORT one.
        if (e == SSL_ERROR_SYSCALL && JSocketPlatform::lastFailureWasTimeout()) return kStalled;
        return (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) ? kClosed : kFailed;
    }
#endif
    const long long n = JSocketPlatform::receiveSome(m_impl->fd, static_cast<char*>(buf), want);
    if (n < 0) return JSocketPlatform::lastFailureWasTimeout() ? kStalled : kFailed;
    return static_cast<int>(n);
}

bool JTcpSocket::waitReadable(int ms) {
    if (!isOpen()) return true;                                   // read() reports it at once
#if JF_TCP_TLS
    if (m_impl->ssl && SSL_pending(m_impl->ssl) > 0) return true;
#endif
    return JSocketPlatform::poll(m_impl->fd, false, ms) != 0;   // ready, or the wait failed: read() says which
}

} // inline namespace jf
