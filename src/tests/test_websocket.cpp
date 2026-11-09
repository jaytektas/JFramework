// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JWebSocket against an in-process server on 127.0.0.1 (a JTcpServer speaking just enough RFC 6455 to
// test the client): the handshake and its accept check, an echo, a message that arrives in fragments
// with a ping in the middle of it, ping/pong both ways, a frame that rode in with the 101, the close
// handshake from either end, and the ways a connection fails to open. Plus the SHA-1 and base64 it
// stands on, against their published vectors.

#include <j/core/MainThreadDispatcher.h>
#include <j/io/JBase64.h>
#include <j/io/JSha1.h>
#include <j/io/JTcpServer.h>
#include <j/io/JTcpSocket.h>
#include <j/io/JWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jf;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

// Run the main thread's queue until `done` or `ms` pass — the signals arrive there.
static bool pumpUntil(const std::function<bool()>& done, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        JMainThreadDispatcher::instance().drainFor(10);
    }
    return true;
}

// ---- The test server --------------------------------------------------------------------------
struct ServerFrame {
    bool        fin{false};
    bool        masked{false};
    uint8_t     opcode{0};
    std::string payload;
};

class TestServer {
public:
    enum class Mode { Normal, WrongAccept, NotAnUpgrade };

    explicit TestServer(Mode mode = Mode::Normal) : m_mode(mode) {
        std::string why;
        m_server.listen(0, why);
        m_thread = std::thread([this] { serve(); });
    }
    ~TestServer() { m_stop = true; m_thread.join(); }

    std::string url() const { return "ws://127.0.0.1:" + std::to_string(m_server.port()) + "/ws?clientId=test"; }

    std::string request()       { std::lock_guard<std::mutex> lk(m_mx); return m_request; }
    bool        allMasked()     { std::lock_guard<std::mutex> lk(m_mx); return m_allMasked; }
    std::string pongPayload()   { std::lock_guard<std::mutex> lk(m_mx); return m_pong; }
    int         closeCodeSeen() { std::lock_guard<std::mutex> lk(m_mx); return m_closeCode; }

private:
    void send(JTcpSocket& c, bool fin, uint8_t opcode, const std::string& payload) {
        std::string f;
        f += char((fin ? 0x80 : 0) | opcode);
        if (payload.size() < 126) {
            f += char(payload.size());
        } else if (payload.size() <= 0xFFFF) {
            f += char(126);
            for (int s = 8; s >= 0; s -= 8) f += char((payload.size() >> s) & 0xFF);
        } else {
            f += char(127);
            for (int s = 56; s >= 0; s -= 8) f += char((uint64_t(payload.size()) >> s) & 0xFF);
        }
        f += payload;
        std::string ignored;
        c.write(f, ignored);
    }

    // One whole frame off the front of `rx`, unmasked; false while it has not all arrived.
    static bool takeFrame(std::string& rx, ServerFrame& f) {
        const auto* p = reinterpret_cast<const uint8_t*>(rx.data());
        if (rx.size() < 2) return false;
        f.fin    = p[0] & 0x80;
        f.opcode = p[0] & 0x0F;
        f.masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t   hdr = 2;
        if (len == 126) {
            if (rx.size() < 4) return false;
            len = uint64_t(p[2]) << 8 | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (rx.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = len << 8 | p[2 + i];
            hdr = 10;
        }
        const size_t maskAt = hdr;
        if (f.masked) hdr += 4;
        if (rx.size() < hdr + len) return false;
        f.payload = rx.substr(hdr, size_t(len));
        if (f.masked)
            for (size_t i = 0; i < f.payload.size(); ++i) f.payload[i] = char(f.payload[i] ^ rx[maskAt + (i & 3)]);
        rx.erase(0, hdr + size_t(len));
        return true;
    }

    bool readFrame(JTcpSocket& c, std::string& rx, ServerFrame& f) {
        char buf[4096];
        while (!takeFrame(rx, f)) {
            if (m_stop) return false;
            if (!c.waitReadable(20)) continue;
            const int n = c.read(buf, sizeof buf);
            if (n == JTcpSocket::kStalled) continue;
            if (n <= 0) return false;
            rx.append(buf, static_cast<size_t>(n));
        }
        return true;
    }

    void serve() {
        while (!m_stop) {
            JTcpSocket c;
            if (!m_server.accept(c, 20, 2000)) continue;
            session(c);
        }
    }

    void session(JTcpSocket& c) {
        std::string rx;
        char buf[4096];
        while (rx.find("\r\n\r\n") == std::string::npos) {
            const int n = c.read(buf, sizeof buf);
            if (n <= 0) return;
            rx.append(buf, static_cast<size_t>(n));
        }
        const std::string head = rx.substr(0, rx.find("\r\n\r\n") + 4);
        rx.erase(0, head.size());
        { std::lock_guard<std::mutex> lk(m_mx); m_request = head; }

        const size_t k = head.find("Sec-WebSocket-Key: ");
        const std::string key = k == std::string::npos ? std::string() : head.substr(k + 19, head.find("\r\n", k) - k - 19);
        const JSha1::Digest d = JSha1::digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string accept = JBase64::encode(d.data(), d.size());
        if (m_mode == Mode::WrongAccept) accept[0] = accept[0] == 'A' ? 'B' : 'A';
        std::string ignored;
        if (m_mode == Mode::NotAnUpgrade) {
            c.write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", ignored);
            return;
        }
        // The 101 and a first frame in ONE write: the client must not lose what followed the head.
        std::string answer = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        answer += char(0x81);
        answer += char(7);
        answer += "welcome";
        c.write(answer, ignored);

        ServerFrame f;
        while (readFrame(c, rx, f)) {
            { std::lock_guard<std::mutex> lk(m_mx); m_allMasked = m_allMasked && f.masked; }
            if (f.opcode == 0x1 && f.payload == "fragment") {
                send(c, false, 0x1, "Hel");
                send(c, false, 0x0, "lo, ");
                send(c, true,  0x9, "mid");                // a ping between fragments: answered at once
                send(c, true,  0x0, "world");
            } else if (f.opcode == 0x1 && f.payload == "close-me") {
                send(c, true, 0x8, std::string("\x03\xE8", 2) + "bye");   // 1000 "bye"
            } else if (f.opcode == 0x1 || f.opcode == 0x2) {
                send(c, true, f.opcode, f.payload);        // echo
            } else if (f.opcode == 0x9) {
                send(c, true, 0xA, f.payload);             // pong with the ping's payload
            } else if (f.opcode == 0xA) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_pong = f.payload;
            } else if (f.opcode == 0x8) {
                {
                    std::lock_guard<std::mutex> lk(m_mx);
                    m_closeCode = f.payload.size() >= 2 ? (uint8_t(f.payload[0]) << 8 | uint8_t(f.payload[1])) : 0;
                }
                send(c, true, 0x8, f.payload.substr(0, 2));   // echo the close, then hang up
                return;
            }
        }
    }

    Mode              m_mode;
    JTcpServer        m_server;
    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
    std::mutex        m_mx;
    std::string       m_request;
    std::string       m_pong;
    bool              m_allMasked{true};
    int               m_closeCode{0};
};

// ---- What the client reported -----------------------------------------------------------------
struct Seen {
    int                      opened{0};
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> binaries;
    std::vector<std::string> pongs;
    int                      closed{0};
    int                      closeCode{0};
    std::string              closeReason;

    void watch(JWebSocket& ws) {
        ws.onOpen.connect([this] { ++opened; });
        ws.onText.connect([this](const std::string& t) { texts.push_back(t); });
        ws.onBinary.connect([this](const std::vector<uint8_t>& b) { binaries.push_back(b); });
        ws.onPong.connect([this](const std::string& p) { pongs.push_back(p); });
        ws.onClosed.connect([this](int code, const std::string& why) { ++closed; closeCode = code; closeReason = why; });
    }
};

// ---- Tests ------------------------------------------------------------------------------------
static void test_digests() {
    CHECK(JSha1::hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(JSha1::hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(JSha1::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(JSha1::hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    CHECK(JBase64::encode("") == "");
    CHECK(JBase64::encode("f") == "Zg==");
    CHECK(JBase64::encode("fo") == "Zm8=");
    CHECK(JBase64::encode("foo") == "Zm9v");
    CHECK(JBase64::encode("foobar") == "Zm9vYmFy");
    // RFC 6455 §1.3's own example of the handshake.
    const JSha1::Digest d = JSha1::digest(std::string("dGhlIHNhbXBsZSBub25jZQ==") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    CHECK(JBase64::encode(d.data(), d.size()) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    std::printf("  [OK] SHA-1 and base64 against FIPS / RFC 4648 / RFC 6455 vectors\n");
}

static void test_conversation() {
    TestServer server;
    JWebSocket ws;
    Seen seen;
    seen.watch(ws);
    CHECK(ws.state() == JWebSocket::State::Closed);
    CHECK(!ws.sendText("too early"));                    // nothing is queued before the socket is open

    ws.open(server.url());
    CHECK(ws.state() == JWebSocket::State::Connecting);
    CHECK(pumpUntil([&] { return seen.opened == 1 && seen.texts.size() == 1; }));
    CHECK(ws.state() == JWebSocket::State::Open);
    CHECK(!seen.texts.empty() && seen.texts[0] == "welcome");   // rode in with the 101
    const std::string req = server.request();
    CHECK(req.rfind("GET /ws?clientId=test HTTP/1.1\r\n", 0) == 0);
    CHECK(req.find("Upgrade: websocket\r\n") != std::string::npos);
    CHECK(req.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    CHECK(req.find("Host: 127.0.0.1:") != std::string::npos);
    std::printf("  [OK] handshake: upgrade request, accept verified, a frame sent with the 101 kept\n");

    CHECK(ws.sendText("hello, server"));
    CHECK(pumpUntil([&] { return seen.texts.size() == 2; }));
    CHECK(seen.texts.size() == 2 && seen.texts[1] == "hello, server");
    const std::string mid(300, 'm'), big(70000, 'b');    // a 16-bit length, and a 64-bit one, both ways
    CHECK(ws.sendText(mid));
    CHECK(ws.sendText(big));
    CHECK(pumpUntil([&] { return seen.texts.size() == 4; }));
    CHECK(seen.texts.size() == 4 && seen.texts[2] == mid && seen.texts[3] == big);
    CHECK(ws.sendBinary({ 0, 1, 2, 255 }));
    CHECK(pumpUntil([&] { return seen.binaries.size() == 1; }));
    CHECK(!seen.binaries.empty() && seen.binaries[0] == std::vector<uint8_t>({ 0, 1, 2, 255 }));
    std::printf("  [OK] text (7-, 16- and 64-bit lengths) and binary echo\n");

    CHECK(ws.sendText("fragment"));
    CHECK(pumpUntil([&] { return seen.texts.size() == 5; }));
    CHECK(seen.texts.size() == 5 && seen.texts[4] == "Hello, world");
    CHECK(pumpUntil([&] { return server.pongPayload() == "mid"; }));
    std::printf("  [OK] a fragmented message reassembled; the ping inside it answered with a pong\n");

    CHECK(ws.ping("are you there"));
    CHECK(pumpUntil([&] { return seen.pongs.size() == 1; }));
    CHECK(!seen.pongs.empty() && seen.pongs[0] == "are you there");
    std::printf("  [OK] ping() answered: onPong carries the payload\n");

    CHECK(server.allMasked());
    std::printf("  [OK] every frame the client sent was masked\n");

    ws.close(JWebSocket::kNormalClosure, "done");
    CHECK(ws.state() == JWebSocket::State::Closing);
    CHECK(!ws.sendText("after close"));
    CHECK(pumpUntil([&] { return seen.closed == 1; }));
    CHECK(seen.closeCode == JWebSocket::kNormalClosure);
    CHECK(server.closeCodeSeen() == JWebSocket::kNormalClosure);
    CHECK(ws.state() == JWebSocket::State::Closed);
    std::printf("  [OK] close(): the server saw 1000 and answered; onClosed(1000) once\n");

    // And again on the same object: a reconnect is just another open().
    ws.open(server.url());
    CHECK(pumpUntil([&] { return seen.opened == 2; }));
    CHECK(ws.sendText("close-me"));
    CHECK(pumpUntil([&] { return seen.closed == 2; }));
    CHECK(seen.closeCode == 1000 && seen.closeReason == "bye");
    CHECK(server.closeCodeSeen() == 1000);               // the client answered the server's close
    std::printf("  [OK] reopen; a close the server starts is answered and reported (1000 \"bye\")\n");
}

static void test_failures_to_open() {
    {
        TestServer server(TestServer::Mode::WrongAccept);
        JWebSocket ws;
        Seen seen;
        seen.watch(ws);
        ws.open(server.url());
        CHECK(pumpUntil([&] { return seen.closed == 1; }));
        CHECK(seen.opened == 0);
        CHECK(seen.closeCode == JWebSocket::kAbnormalClosure);
        CHECK(seen.closeReason.find("Sec-WebSocket-Accept") != std::string::npos);
    }
    {
        TestServer server(TestServer::Mode::NotAnUpgrade);
        JWebSocket ws;
        Seen seen;
        seen.watch(ws);
        ws.open(server.url());
        CHECK(pumpUntil([&] { return seen.closed == 1; }));
        CHECK(seen.opened == 0 && seen.closeReason.find("404") != std::string::npos);
    }
    {
        uint16_t freePort = 0;
        { JTcpServer s; std::string why; s.listen(0, why); freePort = s.port(); }
        JWebSocket ws;
        Seen seen;
        seen.watch(ws);
        ws.open("ws://127.0.0.1:" + std::to_string(freePort) + "/ws");
        CHECK(pumpUntil([&] { return seen.closed == 1; }));
        CHECK(seen.opened == 0 && seen.closeCode == JWebSocket::kAbnormalClosure);
        CHECK(ws.state() == JWebSocket::State::Closed);
    }
    {
        JWebSocket ws;
        Seen seen;
        seen.watch(ws);
        ws.open("http://127.0.0.1/ws");
        CHECK(pumpUntil([&] { return seen.closed == 1; }));
        CHECK(seen.closeReason.find("not a ws://") != std::string::npos);
    }
    std::printf("  [OK] a wrong accept, a refused upgrade, nothing listening and a bad url each end in onClosed(1006, why)\n");
}

static void test_destroying_an_open_socket() {
    TestServer server;
    Seen seen;
    {
        JWebSocket ws;
        seen.watch(ws);
        ws.open(server.url());
        CHECK(pumpUntil([&] { return seen.opened == 1; }));
    }                                                    // destroyed while open
    pumpUntil([] { return false; }, 100);
    CHECK(seen.closed == 0);                             // nothing fired into the object that is gone
    CHECK(pumpUntil([&] { return server.closeCodeSeen() == JWebSocket::kGoingAway; }));
    std::printf("  [OK] destroying an open socket tells the server 1001 and fires nothing afterwards\n");
}

int main() {
    std::printf("=== JWebSocket ===\n");
    test_digests();
    test_conversation();
    test_failures_to_open();
    test_destroying_an_open_socket();
    std::printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
