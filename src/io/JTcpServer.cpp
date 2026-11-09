// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/JTcpServer.h>

#include <j/io/JTcpSocket.h>

#include "JSocketPlatform.h"

inline namespace jf {

namespace {
constexpr int kBacklog = 16;      // connections the system holds while one is being served
}

JTcpServer::~JTcpServer() { close(); }

bool JTcpServer::listen(uint16_t port, std::string& error) {
    close();
    JSocketPlatform::startup();
    const JSocketPlatform::Handle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == JSocketPlatform::kNone) { error = "cannot create a socket"; return false; }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only: never reachable from another machine
    addr.sin_port        = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, kBacklog) != 0) {
        JSocketPlatform::closeHandle(s);
        error = "cannot listen on 127.0.0.1:" + std::to_string(port);
        return false;
    }
    socklen_t len = sizeof addr;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port   = ntohs(addr.sin_port);
    m_listen = JSocketPlatform::toOpaque(s);
    return true;
}

bool JTcpServer::accept(JTcpSocket& client, int waitMs, int clientTimeoutMs) {
    if (!isListening()) return false;
    const JSocketPlatform::Handle listener = JSocketPlatform::fromOpaque(m_listen);
    if (JSocketPlatform::poll(listener, false, waitMs) <= 0) return false;
    const JSocketPlatform::Handle c = ::accept(listener, nullptr, nullptr);
    if (c == JSocketPlatform::kNone) return false;
    client.adopt(JSocketPlatform::toOpaque(c), clientTimeoutMs);
    return true;
}

void JTcpServer::close() {
    if (!isListening()) return;
    JSocketPlatform::closeHandle(JSocketPlatform::fromOpaque(m_listen));
    m_listen = -1;
    m_port   = 0;
}

} // inline namespace jf
