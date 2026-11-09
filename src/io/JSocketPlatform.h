// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JSocketPlatform — the BSD-socket / Winsock differences, in one place, private to j_platform.
//
// JTcpSocket and JTcpServer are written once against this; nothing outside src/io includes it, so no
// public header ever sees a SOCKET, a winsock2.h or a sys/socket.h. It is a set of static operations
// on the platform's own handle type, not an object — the handle's owner is JTcpSocket or JTcpServer.

#include <mutex>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

inline namespace jf {

class JSocketPlatform {
public:
#if defined(_WIN32)
    using Handle = SOCKET;
    static constexpr Handle kNone = INVALID_SOCKET;
#else
    using Handle = int;
    static constexpr Handle kNone = -1;
#endif

    // Winsock must be started once per process before the first socket call; POSIX has nothing to do.
    static void startup() {
#if defined(_WIN32)
        static std::once_flag once;
        std::call_once(once, [] { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); });
#endif
    }

    static void closeHandle(Handle h) {
        if (h == kNone) return;
#if defined(_WIN32)
        ::closesocket(h);
#else
        ::close(h);
#endif
    }

    // The handle as an integer and back — how a public header (JTcpSocket's adopt()) carries one
    // without naming the platform's type.
    static long long toOpaque(Handle h)       { return static_cast<long long>(h); }
    static Handle    fromOpaque(long long v)  { return static_cast<Handle>(v); }

    // Wait up to `ms` for `h` to become readable (or, with `forWrite`, writable). >0 ready — which
    // includes the peer hanging up or an error pending, both of which a read then reports — 0 when the
    // wait ran out, <0 when the wait itself failed.
    static int poll(Handle h, bool forWrite, int ms) {
#if defined(_WIN32)
        WSAPOLLFD p{ h, static_cast<SHORT>(forWrite ? POLLWRNORM : POLLRDNORM), 0 };
        return ::WSAPoll(&p, 1, ms);
#else
        pollfd p{ h, static_cast<short>(forWrite ? POLLOUT : POLLIN), 0 };
        return ::poll(&p, 1, ms);
#endif
    }

    static void setNonBlocking(Handle h, bool on) {
#if defined(_WIN32)
        u_long mode = on ? 1 : 0;
        ::ioctlsocket(h, FIONBIO, &mode);
#else
        const int flags = ::fcntl(h, F_GETFL, 0);
        ::fcntl(h, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
    }

    // The longest a blocking send or receive on `h` may wait before it gives up.
    static void setTimeouts(Handle h, int ms) {
#if defined(_WIN32)
        const DWORD tv = static_cast<DWORD>(ms);
#else
        const timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#endif
        ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(h, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    static void setNoDelay(Handle h) {
        int one = 1;
        ::setsockopt(h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    }

    // Did the call that just failed fail because its wait ran out (a receive timeout), rather than
    // because the connection broke?
    static bool lastFailureWasTimeout() {
#if defined(_WIN32)
        const int e = ::WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    // Did a non-blocking connect() that just returned an error merely start, rather than fail?
    static bool lastConnectInProgress() {
#if defined(_WIN32)
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EINPROGRESS;
#endif
    }

    // The outcome of a non-blocking connect once the handle has turned writable: 0 when connected.
    static int pendingError(Handle h) {
        int err = 0;
#if defined(_WIN32)
        int len = sizeof(err);
#else
        socklen_t len = sizeof(err);
#endif
        if (::getsockopt(h, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return -1;
        return err;
    }

    // send() that never raises SIGPIPE on a peer that has gone: the failure comes back as a return
    // value, where the caller can deal with it, instead of killing the process.
    static long long sendSome(Handle h, const char* data, size_t len) {
#if defined(_WIN32)
        return ::send(h, data, static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
        return ::send(h, data, len, MSG_NOSIGNAL);
#else
        return ::send(h, data, len, 0);
#endif
    }

    static long long receiveSome(Handle h, char* buf, size_t cap) {
#if defined(_WIN32)
        return ::recv(h, buf, static_cast<int>(cap), 0);
#else
        return ::recv(h, buf, cap, 0);
#endif
    }
};

} // inline namespace jf
