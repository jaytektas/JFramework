// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JTcpSocket — one TCP connection, optionally wrapped in TLS, with every wait bounded.
//
// The connection JHttpClient and JWebSocket speak over, and what JTcpServer hands back for each client
// it accepts. The header carries no platform or TLS types: BSD sockets / Winsock and OpenSSL live in
// JTcpSocket.cpp behind an opaque Impl.
//
// TLS: OpenSSL on POSIX, verifying the certificate chain against the system store AND the host name —
// a client that skips either is a plaintext client with extra steps. A build without OpenSSL, and
// Windows (whose HTTPS goes through WinHTTP inside JHttpClient instead), answer a TLS connect with an
// error saying so; a secure connection is never quietly made as a plaintext one. tlsAvailable() says
// which kind of build this is.
//
// BLOCKING, WITH LIMITS. connect() gives up after its timeout; after that, any one wait for the peer
// — a read with nothing arriving, a write the peer will not take — gives up after timeout() and says
// so (kStalled), so a connection that simply goes quiet never hangs its caller. waitReadable() lets a
// loop that must also do other work (JWebSocket's, sending what the main thread queued) poll instead.
//
// One thread at a time: a socket is used by the thread that owns it. It is not shared.
//
//     JTcpSocket s;
//     std::string why;
//     if (!s.connect("example.com", 443, true, 5000, why)) fail(why);
//     s.write("GET / HTTP/1.1\r\n...", why);
//     char buf[4096];
//     const int n = s.read(buf, sizeof buf);    // >0 bytes, 0 closed, kStalled, kFailed

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

inline namespace jf {

class JTcpServer;

class JTcpSocket {
public:
    // read() outcomes other than a byte count.
    static constexpr int kClosed  = 0;     // the peer closed the connection cleanly
    static constexpr int kFailed  = -1;    // the connection broke
    static constexpr int kStalled = -2;    // nothing arrived within timeout()

    JTcpSocket();
    ~JTcpSocket();

    JTcpSocket(const JTcpSocket&)            = delete;
    JTcpSocket& operator=(const JTcpSocket&) = delete;

    // Connect to host:port — every address the name resolves to, in turn — and, with `tls`, complete a
    // verified TLS handshake for `host`. `timeoutMs` bounds each attempt and the handshake, and becomes
    // timeout(). An open socket is closed first. False with the reason in `error`.
    bool connect(const std::string& host, uint16_t port, bool tls, int timeoutMs, std::string& error);

    bool isOpen() const;
    bool isSecure() const;

    // The longest any one wait for the peer may take.
    void setTimeout(int ms);
    int  timeout() const;

    // Write all of `data`, or fail (connection broken, or the peer took nothing for timeout()).
    bool write(const void* data, size_t len, std::string& error);
    bool write(const std::string& data, std::string& error) { return write(data.data(), data.size(), error); }

    // Read what has arrived, up to `cap` bytes, waiting up to timeout() for the first of it. Returns the
    // byte count, or kClosed / kFailed / kStalled.
    int read(void* buf, size_t cap);

    // Wait up to `ms` for read() to have something to report without waiting — data, or the peer
    // closing, or a failure. Bytes TLS has already decrypted count, though the socket itself is idle.
    bool waitReadable(int ms);

    // Close the connection (a TLS session is shut down first). Safe on a socket that is not open.
    void close();

    // True when this build can make a TLS connection.
    static bool tlsAvailable();

private:
    friend class JTcpServer;
    // Take ownership of an already-connected platform handle (JTcpServer::accept), as an integer so
    // that this header never names the platform's socket type.
    void adopt(long long handle, int timeoutMs);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // inline namespace jf
