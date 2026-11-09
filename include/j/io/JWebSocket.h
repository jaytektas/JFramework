// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JWebSocket — a WebSocket client (RFC 6455) over a JTcpSocket: ws://, and wss:// where JTcpSocket has
// TLS (JTcpSocket::tlsAvailable()).
//
// open() returns at once; the connection, the HTTP Upgrade handshake (a random Sec-WebSocket-Key, the
// server's Sec-WebSocket-Accept checked against it) and every frame after it run on the socket's own
// thread. What arrives is delivered through the signals below ON THE MAIN THREAD, via
// JMainThreadDispatcher, so a slot may touch widgets directly. Messages arrive whole: a message the
// server sent in fragments is reassembled first, and a text message is checked to be UTF-8.
//
// The protocol's housekeeping is the socket's: every frame it sends is masked, a ping is answered
// with a pong, and a close from the server is answered and reported. close() runs the closing
// handshake. A connection that breaks, or never opens, ends the same way — onClosed, with
// kAbnormalClosure and the reason — exactly once for every open(). Reconnecting is the caller's
// decision (when, how often, whether at all), made from onClosed.
//
// No extensions (permessage-deflate) and no subprotocols are negotiated; a server that imposes either
// is refused, as the RFC requires.
//
//     JWebSocket ws;
//     ws.onOpen.connect([&] { ws.sendText("hello"); });
//     ws.onText.connect([](const std::string& msg) { show(msg); });
//     ws.onClosed.connect([](int code, const std::string& why) { retryLater(); });
//     ws.open("ws://127.0.0.1:8188/ws?clientId=me");

#include <j/core/Signal.h>
#include <j/io/JHttpHeader.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JWebSocket {
public:
    enum class State { Closed, Connecting, Open, Closing };

    // Close codes (RFC 6455 §7.4.1) the socket itself sends or reports.
    static constexpr int kNormalClosure   = 1000;
    static constexpr int kGoingAway       = 1001;   // sent when the socket is destroyed while open
    static constexpr int kProtocolError   = 1002;
    static constexpr int kNoStatus        = 1005;   // reported: the server's close carried no code
    static constexpr int kAbnormalClosure = 1006;   // reported: no closing handshake — it broke, or never opened
    static constexpr int kInvalidData     = 1007;   // a text message that is not UTF-8
    static constexpr int kMessageTooBig   = 1009;

    JWebSocket();
    ~JWebSocket();

    JWebSocket(const JWebSocket&)            = delete;
    JWebSocket& operator=(const JWebSocket&) = delete;

    // Connect to a ws:// or wss:// url, sending `headers` with the upgrade request. A socket that is
    // still connected is closed first (its onClosed comes before this connection's signals).
    void open(const std::string& url, const std::vector<JHttpHeader>& headers = {});

    // Queue a message for the server. False — and nothing is sent — unless the socket is Open.
    bool sendText(const std::string& text);
    bool sendBinary(const std::vector<uint8_t>& data);
    bool ping(const std::string& payload = {});

    // Start the closing handshake: the server's reply (or the timeout) ends the connection and onClosed
    // reports it. A socket still connecting gives up instead.
    void close(int code = kNormalClosure, const std::string& reason = {});

    State state() const;

    // How long connecting, the opening handshake and the closing handshake may each take. Applies to
    // the next open().
    void setTimeout(int ms);
    int  timeout() const;

    // All signals fire on the main thread.
    JSignal<>                     onOpen;      // the server accepted the upgrade
    JSignal<std::string>          onText;      // a whole text message, UTF-8
    JSignal<std::vector<uint8_t>> onBinary;    // a whole binary message
    JSignal<std::string>          onPong;      // a pong's payload — the answer to ping()
    JSignal<int, std::string>     onClosed;    // close code and reason; once per open()

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // inline namespace jf
