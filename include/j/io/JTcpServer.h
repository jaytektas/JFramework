// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JTcpServer — a listening TCP socket on the loopback interface, handing each connection it accepts
// over as a JTcpSocket.
//
// Loopback only, deliberately: what the framework serves (JLocalWebServer's pages to the user's own
// browser, a test's stand-in server) is for this machine and must never be reachable from another.
// The header carries no platform types; the listening handle lives in JTcpServer.cpp.
//
//     JTcpServer server;
//     std::string why;
//     if (!server.listen(0, why)) fail(why);         // 0: the system picks a free port
//     JTcpSocket client;
//     if (server.accept(client, 200, 2000)) serve(client);

#include <cstdint>
#include <string>

inline namespace jf {

class JTcpSocket;

class JTcpServer {
public:
    JTcpServer() = default;
    ~JTcpServer();

    JTcpServer(const JTcpServer&)            = delete;
    JTcpServer& operator=(const JTcpServer&) = delete;

    // Listen on 127.0.0.1:`port` (0: a port the system picks — port() says which). Listening again
    // closes the previous socket first. False with the reason in `error`.
    bool listen(uint16_t port, std::string& error);

    // Wait up to `waitMs` for a connection. True with `client` now owning it, its timeout() set to
    // `clientTimeoutMs`; false when none arrived (or the server is not listening).
    bool accept(JTcpSocket& client, int waitMs, int clientTimeoutMs);

    void     close();
    bool     isListening() const { return m_listen >= 0; }
    uint16_t port() const        { return m_port; }

private:
    long long m_listen = -1;      // the listening socket, as the platform's handle
    uint16_t  m_port   = 0;
};

} // inline namespace jf
