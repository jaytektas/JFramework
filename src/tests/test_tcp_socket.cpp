// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JTcpSocket / JTcpServer: a loopback connection both ways, every wait bounded (a quiet peer is a
// stall, not a hang), a hang-up reported as one, a connection that cannot be made reported with the
// reason, and TLS that fails rather than falls back to plaintext.

#include <j/io/JTcpServer.h>
#include <j/io/JTcpSocket.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

using Clock = std::chrono::steady_clock;
static long long msSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Read exactly `n` bytes, or fewer if the connection ends or stalls.
static std::string readN(JTcpSocket& s, size_t n) {
    std::string out;
    char buf[65536];
    while (out.size() < n) {
        const int got = s.read(buf, std::min(sizeof buf, n - out.size()));
        if (got <= 0) break;
        out.append(buf, static_cast<size_t>(got));
    }
    return out;
}

static void test_round_trip() {
    JTcpServer server;
    std::string why;
    CHECK(server.listen(0, why));
    CHECK(server.isListening() && server.port() > 0);

    JTcpSocket client;
    CHECK(client.connect("127.0.0.1", server.port(), false, 2000, why));
    CHECK(client.isOpen() && !client.isSecure());
    CHECK(client.timeout() == 2000);

    JTcpSocket peer;
    CHECK(server.accept(peer, 2000, 2000));
    CHECK(peer.isOpen());

    CHECK(!client.waitReadable(20));                     // nothing sent yet: nothing to read
    CHECK(client.write("ping", why));
    CHECK(readN(peer, 4) == "ping");
    CHECK(peer.write("pong", why));
    CHECK(client.waitReadable(1000));
    CHECK(readN(client, 4) == "pong");

    // A megabyte one way: partial sends and short reads are the socket's business, not the caller's.
    std::string big(1 << 20, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char(i * 7 + (i >> 9));
    std::string got;
    std::thread reader([&] { got = readN(peer, big.size()); });
    CHECK(client.write(big, why));
    reader.join();
    CHECK(got == big);
    std::printf("  [OK] round trip both ways, and a megabyte intact\n");
}

static void test_a_quiet_peer_is_a_stall_not_a_hang() {
    JTcpServer server;
    std::string why;
    CHECK(server.listen(0, why));
    JTcpSocket client, peer;
    CHECK(client.connect("127.0.0.1", server.port(), false, 2000, why));
    CHECK(server.accept(peer, 2000, 2000));

    client.setTimeout(200);
    char buf[16];
    const auto t0 = Clock::now();
    CHECK(client.read(buf, sizeof buf) == JTcpSocket::kStalled);
    const long long ms = msSince(t0);
    CHECK(ms >= 150 && ms < 1500);
    std::printf("  [OK] a read with nothing arriving gives up after its timeout (%lld ms)\n", ms);

    peer.close();
    CHECK(client.waitReadable(1000));                    // the hang-up is something to read
    CHECK(client.read(buf, sizeof buf) == JTcpSocket::kClosed);
    std::printf("  [OK] the peer hanging up reads as closed\n");
}

static void test_accept_waits_only_as_long_as_asked() {
    JTcpServer server;
    std::string why;
    CHECK(server.listen(0, why));
    JTcpSocket nobody;
    const auto t0 = Clock::now();
    CHECK(!server.accept(nobody, 100, 1000));
    CHECK(msSince(t0) < 1000);
    CHECK(!nobody.isOpen());
    server.close();
    CHECK(!server.isListening());
    CHECK(!server.accept(nobody, 100, 1000));            // not listening: nothing, at once
    std::printf("  [OK] accept with nobody calling returns after its wait\n");
}

static void test_failures_say_why() {
    std::string why;
    uint16_t freePort = 0;
    {
        JTcpServer s;
        CHECK(s.listen(0, why));
        freePort = s.port();
    }                                                    // closed: nothing listens there now
    JTcpSocket c;
    why.clear();
    CHECK(!c.connect("127.0.0.1", freePort, false, 2000, why));
    CHECK(why.find("cannot connect") != std::string::npos);
    CHECK(!c.isOpen());

    why.clear();
    CHECK(!c.connect("no-such-host.invalid", 80, false, 2000, why));
    CHECK(why.find("cannot resolve") != std::string::npos);

    char buf[4];
    CHECK(c.read(buf, sizeof buf) == JTcpSocket::kFailed);
    CHECK(!c.write("x", why));
    std::printf("  [OK] a refused connect and an unknown host fail with the reason\n");
}

static void test_tls_never_falls_back_to_plaintext() {
    // A plaintext server answering a TLS ClientHello with HTTP. The connect must FAIL — a "secure"
    // connection that quietly carried on in the clear would be worse than none.
    JTcpServer server;
    std::string why;
    CHECK(server.listen(0, why));
    std::thread t([&] {
        JTcpSocket peer;
        if (!server.accept(peer, 2000, 2000)) return;
        char buf[1024];
        (void)peer.read(buf, sizeof buf);
        std::string ignored;
        peer.write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", ignored);
    });
    JTcpSocket c;
    why.clear();
    CHECK(!c.connect("127.0.0.1", server.port(), true, 2000, why));
    t.join();
    CHECK(!why.empty());
    CHECK(!c.isOpen());
    std::printf("  [OK] TLS to a plaintext server fails (%s); tlsAvailable() = %s\n", why.c_str(),
                JTcpSocket::tlsAvailable() ? "true" : "false");
}

int main() {
    std::printf("=== JTcpSocket / JTcpServer ===\n");
    test_round_trip();
    test_a_quiet_peer_is_a_stall_not_a_hang();
    test_accept_waits_only_as_long_as_asked();
    test_failures_say_why();
    test_tls_never_falls_back_to_plaintext();
    std::printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
