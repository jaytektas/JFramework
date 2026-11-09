// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/io/JWebSocket.h>

#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/io/JBase64.h>
#include <j/io/JHttpHead.h>
#include <j/io/JSha1.h>
#include <j/io/JTcpSocket.h>
#include <j/io/JUrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

inline namespace jf {

namespace {

constexpr const char* kLog              = "io.ws";
constexpr const char* kAcceptGuid       = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";   // RFC 6455 §1.3
constexpr int         kDefaultTimeoutMs = 10000;
constexpr int         kPollMs           = 20;       // how soon a queued frame goes out, and a close() is seen
constexpr size_t      kKeyBytes         = 16;       // Sec-WebSocket-Key: 16 random bytes, base64
constexpr size_t      kMaxHeadBytes     = 16384;    // an upgrade response longer than this is not one
constexpr uint64_t    kMaxMessageBytes  = uint64_t(64) << 20;   // one message, reassembled
constexpr size_t      kMaxControlBytes  = 125;      // RFC 6455 §5.5
constexpr size_t      kMaxFrameHeader   = 14;       // 2 + a 64-bit length + the mask

enum Opcode : uint8_t { kContinuation = 0x0, kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA };

using Clock = std::chrono::steady_clock;

// Unpredictable bytes, as the RFC asks of both the handshake key and every frame's mask. Each thread
// keeps its own device: the main thread masks what it queues, the socket's thread what it answers.
void randomBytes(uint8_t* out, size_t n) {
    thread_local std::random_device rd;
    for (size_t i = 0; i < n; i += 4) {
        const uint32_t v = rd();
        for (size_t k = 0; k < 4 && i + k < n; ++k) out[i + k] = uint8_t(v >> (8 * k));
    }
}

// One client-to-server frame: FIN set (nothing here sends in fragments), masked.
std::string encodeFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    std::string f;
    f.reserve(len + kMaxFrameHeader);
    f += char(0x80 | opcode);
    if (len < 126) {
        f += char(0x80 | len);
    } else if (len <= 0xFFFF) {
        f += char(0x80 | 126);
        f += char(len >> 8);
        f += char(len & 0xFF);
    } else {
        f += char(0x80 | 127);
        for (int s = 56; s >= 0; s -= 8) f += char((uint64_t(len) >> s) & 0xFF);
    }
    uint8_t mask[4];
    randomBytes(mask, sizeof mask);
    f.append(reinterpret_cast<const char*>(mask), sizeof mask);
    for (size_t i = 0; i < len; ++i) f += char(data[i] ^ mask[i & 3]);
    return f;
}
std::string encodeFrame(uint8_t opcode, const std::string& payload) {
    return encodeFrame(opcode, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

std::string closePayload(int code, const std::string& reason) {
    std::string p;
    p += char((code >> 8) & 0xFF);
    p += char(code & 0xFF);
    p += reason.substr(0, kMaxControlBytes - 2);
    return p;
}

// Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing past U+10FFFF.
bool validUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n; ) {
        const unsigned char c = p[i];
        size_t len; uint32_t cp, min;
        if      (c < 0x80)           { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

bool hasToken(const std::string& headerValue, const std::string& token) {
    size_t p = 0;
    while (p <= headerValue.size()) {
        const size_t comma = std::min(headerValue.find(',', p), headerValue.size());
        std::string item = headerValue.substr(p, comma - p);
        const size_t b = item.find_first_not_of(" \t");
        const size_t e = item.find_last_not_of(" \t");
        item = b == std::string::npos ? std::string() : item.substr(b, e - b + 1);
        if (JHttpHeader::sameName(item, token)) return true;
        p = comma + 1;
    }
    return false;
}

// One frame from the server, unmasked.
struct Frame {
    bool        fin{false};
    uint8_t     opcode{0};
    std::string payload;
};

enum class Take { Incomplete, Frame, Bad };

// What a frame to the server is, besides its bytes: a close carries the code it sent.
struct Outgoing {
    std::string frame;
    int         closeCode{0};     // 0: not a close
};

// A code a close frame may carry on the wire (RFC 6455 §7.4): 1005, 1006 and 1015 only ever describe a
// closure, and the rest below 3000 are the RFC's to assign.
bool validCloseCode(int c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

} // namespace

struct JWebSocket::Impl {
    JWebSocket&                        owner;
    // Cleared by the destructor: a callback still queued on the dispatcher then fires nothing.
    std::shared_ptr<std::atomic<bool>> alive{ std::make_shared<std::atomic<bool>>(true) };
    std::atomic<State>                 state{State::Closed};
    std::atomic<bool>                  stop{false};       // the owner is going (or reopening): end now
    int                                timeoutMs{kDefaultTimeoutMs};
    std::thread                        thread;

    std::mutex                         outMutex;
    std::deque<Outgoing>               outgoing;          // encoded frames, for the socket's thread to write
    bool                               closeQueued{false};

    explicit Impl(JWebSocket& o) : owner(o) {}

    // ---- Delivery to the main thread ------------------------------------------------------------
    template <typename F>
    void post(F&& f) {
        auto a = alive;
        JMainThreadDispatcher::instance().post([a, f = std::forward<F>(f)]() mutable { if (a->load()) f(); });
    }

    void finish(JTcpSocket& sock, int code, const std::string& reason) {
        sock.close();
        // A connection that never opened is a failed attempt, not a lost connection: worth saying, not
        // worth a warning every time a caller retries a server that is down.
        const bool wasOpen = state.exchange(State::Closed) != State::Connecting;
        JLOGC(kLog, code == kNormalClosure || !wasOpen ? JLogLevel::Info : JLogLevel::Warn)
            << (wasOpen ? "closed (" : "did not open (") << code << (reason.empty() ? "" : ": " + reason) << ")";
        JWebSocket* o = &owner;
        post([o, code, reason] { o->onClosed.emit(code, reason); });
    }

    // ---- Queue (main thread) --------------------------------------------------------------------
    bool queue(std::string frame) {
        if (state.load() != State::Open) return false;
        std::lock_guard<std::mutex> lk(outMutex);
        if (closeQueued) return false;
        outgoing.push_back({ std::move(frame), 0 });
        return true;
    }

    // ---- The socket's thread --------------------------------------------------------------------
    void run(JUrl url, std::vector<JHttpHeader> headers, int timeout) {
        JTcpSocket sock;
        std::string err;
        JLOGC(kLog, JLogLevel::Info) << "connecting to " << url.origin() << url.path;
        if (!sock.connect(url.host, url.port, url.secure, timeout, err)) { finish(sock, kAbnormalClosure, err); return; }

        std::string rx;
        if (!handshake(sock, url, headers, timeout, rx, err)) { finish(sock, kAbnormalClosure, err); return; }
        state.store(State::Open);
        JLOGC(kLog, JLogLevel::Info) << "open: " << url.origin() << url.path;
        JWebSocket* o = &owner;
        post([o] { o->onOpen.emit(); });

        std::string             message;          // a fragmented message, so far
        uint8_t                 messageOpcode = 0;
        int                     closeSent = 0;    // the code of the close we sent; 0 before we send one
        Clock::time_point       closeDeadline{};
        char                    buf[16384];

        // Fail the connection (§7.1.7): tell the server why, then drop it.
        auto fail = [&](int code, const std::string& why) {
            if (!closeSent) sock.write(encodeFrame(kClose, closePayload(code, why)), err);
            finish(sock, code, why);
        };

        for (;;) {
            if (stop.load()) {                    // destroyed or reopened: say so, don't wait for an answer
                if (!closeSent) sock.write(encodeFrame(kClose, closePayload(kGoingAway, {})), err);
                finish(sock, kGoingAway, "closed by the application");
                return;
            }

            std::deque<Outgoing> batch;
            {
                std::lock_guard<std::mutex> lk(outMutex);
                batch.swap(outgoing);
            }
            for (const Outgoing& out : batch) {
                if (closeSent) break;             // nothing goes after a close
                if (!sock.write(out.frame, err)) { finish(sock, kAbnormalClosure, err); return; }
                if (out.closeCode) {
                    closeSent     = out.closeCode;
                    closeDeadline = Clock::now() + std::chrono::milliseconds(timeout);
                }
            }
            if (closeSent && Clock::now() > closeDeadline) {
                finish(sock, kAbnormalClosure, "the server did not answer the close (" + std::to_string(closeSent) + ")");
                return;
            }

            // Every whole frame received so far — the first may have come in with the upgrade response.
            size_t at = 0;
            for (;;) {
                Frame f;
                std::string why;
                int badCode = kProtocolError;
                const Take t = take(rx, at, f, why, badCode);
                if (t == Take::Incomplete) break;
                if (t == Take::Bad) { fail(badCode, why); return; }

                if (f.opcode >= kClose) {                         // control: may arrive mid-message
                    if (f.opcode == kPing) {
                        if (!closeSent && !sock.write(encodeFrame(kPong, f.payload), err)) {
                            finish(sock, kAbnormalClosure, err);
                            return;
                        }
                    } else if (f.opcode == kPong) {
                        post([o, p = std::move(f.payload)] { o->onPong.emit(p); });
                    } else {                                      // close
                        int code = kNoStatus;
                        std::string reason;
                        if (f.payload.size() == 1) { fail(kProtocolError, "a close frame with a one-byte body"); return; }
                        if (f.payload.size() >= 2) {
                            code   = uint8_t(f.payload[0]) << 8 | uint8_t(f.payload[1]);
                            reason = f.payload.substr(2);
                            if (!validCloseCode(code)) { fail(kProtocolError, "close code " + std::to_string(code) + " is not one a peer may send"); return; }
                            if (!validUtf8(reason))    { fail(kInvalidData, "a close reason that is not UTF-8"); return; }
                        }
                        if (!closeSent)                           // the server started it: answer in kind
                            sock.write(encodeFrame(kClose, code == kNoStatus ? std::string() : closePayload(code, {})), err);
                        finish(sock, code, reason);
                        return;
                    }
                    continue;
                }

                if (f.opcode == kContinuation) {
                    if (messageOpcode == 0) { fail(kProtocolError, "a continuation frame with no message to continue"); return; }
                } else {
                    if (messageOpcode != 0) { fail(kProtocolError, "a new message inside a fragmented one"); return; }
                    messageOpcode = f.opcode;
                }
                if (message.size() + f.payload.size() > kMaxMessageBytes) { fail(kMessageTooBig, "message too big"); return; }
                message += f.payload;
                if (!f.fin) continue;

                if (messageOpcode == kText) {
                    if (!validUtf8(message)) { fail(kInvalidData, "a text message that is not UTF-8"); return; }
                    post([o, m = std::move(message)] { o->onText.emit(m); });
                } else {
                    post([o, m = std::vector<uint8_t>(message.begin(), message.end())] { o->onBinary.emit(m); });
                }
                message.clear();
                messageOpcode = 0;
            }
            rx.erase(0, at);

            if (!sock.waitReadable(kPollMs)) continue;
            const int n = sock.read(buf, sizeof buf);
            if (n == JTcpSocket::kStalled) continue;
            if (n == JTcpSocket::kClosed || n == JTcpSocket::kFailed) {
                finish(sock, kAbnormalClosure, n == JTcpSocket::kClosed ? "the server closed the connection"
                                                                        : "the connection broke");
                return;
            }
            rx.append(buf, static_cast<size_t>(n));
        }
    }

    // Send the upgrade request and read the answer. On success `rx` holds whatever followed the head —
    // the server may have sent its first frames in the same packet.
    bool handshake(JTcpSocket& sock, const JUrl& url, const std::vector<JHttpHeader>& headers, int timeout,
                   std::string& rx, std::string& err) {
        uint8_t nonce[kKeyBytes];
        randomBytes(nonce, sizeof nonce);
        const std::string key = JBase64::encode(nonce, sizeof nonce);

        std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.authority() + "\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
        bool haveUA = false;
        for (const auto& h : headers) {
            if (JHttpHeader::sameName(h.name, "User-Agent")) haveUA = true;
            req += h.name + ": " + h.value + "\r\n";
        }
        if (!haveUA) req += "User-Agent: JFramework/1.0\r\n";
        req += "\r\n";
        if (!sock.write(req, err)) return false;

        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
        JHttpHead head;
        size_t bodyAt = std::string::npos;
        char buf[4096];
        while ((bodyAt = head.parse(rx)) == std::string::npos) {
            if (stop.load())             { err = "closed before the connection opened"; return false; }
            if (Clock::now() > deadline) { err = "no answer to the upgrade request within " + std::to_string(timeout) + " ms"; return false; }
            if (rx.size() > kMaxHeadBytes) { err = "the upgrade response is not HTTP"; return false; }
            if (!sock.waitReadable(kPollMs)) continue;
            const int n = sock.read(buf, sizeof buf);
            if (n == JTcpSocket::kStalled) continue;
            if (n <= 0) { err = "the server closed the connection during the upgrade"; return false; }
            rx.append(buf, static_cast<size_t>(n));
        }
        rx.erase(0, bodyAt);

        if (head.status != 101) { err = "the server refused the upgrade (HTTP " + std::to_string(head.status) + ")"; return false; }
        if (!JHttpHeader::sameName(head.header("Upgrade"), "websocket") || !hasToken(head.header("Connection"), "upgrade")) {
            err = "the server's 101 is not a WebSocket upgrade";
            return false;
        }
        const JSha1::Digest d = JSha1::digest(key + kAcceptGuid);
        if (head.header("Sec-WebSocket-Accept") != JBase64::encode(d.data(), d.size())) {
            err = "the server's Sec-WebSocket-Accept does not match the key";
            return false;
        }
        if (!head.header("Sec-WebSocket-Extensions").empty() || !head.header("Sec-WebSocket-Protocol").empty()) {
            err = "the server imposed an extension or subprotocol that was not asked for";
            return false;
        }
        return true;
    }

    // Take one frame from `rx` at `at`, advancing `at` past it.
    // A frame the protocol forbids is Bad, with the reason and the close code it earns.
    static Take take(const std::string& rx, size_t& at, Frame& f, std::string& why, int& badCode) {
        const auto* p = reinterpret_cast<const uint8_t*>(rx.data()) + at;
        const size_t avail = rx.size() - at;
        if (avail < 2) return Take::Incomplete;
        f.fin    = (p[0] & 0x80) != 0;
        f.opcode = p[0] & 0x0F;
        if (p[0] & 0x70) { why = "reserved bits set with no extension negotiated"; return Take::Bad; }
        if (p[1] & 0x80) { why = "a masked frame from the server"; return Take::Bad; }
        const bool control = f.opcode >= kClose;
        if (f.opcode != kContinuation && f.opcode != kText && f.opcode != kBinary &&
            f.opcode != kClose && f.opcode != kPing && f.opcode != kPong) {
            why = "unknown opcode " + std::to_string(f.opcode);
            return Take::Bad;
        }
        uint64_t len = p[1] & 0x7F;
        size_t   hdr = 2;
        if (len == 126) {
            if (avail < 4) return Take::Incomplete;
            len = uint64_t(p[2]) << 8 | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) return Take::Incomplete;
            len = 0;
            for (int i = 0; i < 8; ++i) len = len << 8 | p[2 + i];
            hdr = 10;
        }
        if (control && (!f.fin || len > kMaxControlBytes)) { why = "a fragmented or oversized control frame"; return Take::Bad; }
        if (len > kMaxMessageBytes) { why = "message too big"; badCode = kMessageTooBig; return Take::Bad; }
        if (avail < hdr + len) return Take::Incomplete;
        f.payload.assign(rx, at + hdr, static_cast<size_t>(len));
        at += hdr + static_cast<size_t>(len);
        return Take::Frame;
    }

    void join() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        stop.store(false);
    }
};

JWebSocket::JWebSocket() : m_impl(std::make_unique<Impl>(*this)) {}

JWebSocket::~JWebSocket() {
    m_impl->alive->store(false);
    m_impl->join();
}

void JWebSocket::open(const std::string& url, const std::vector<JHttpHeader>& headers) {
    m_impl->join();
    {
        std::lock_guard<std::mutex> lk(m_impl->outMutex);
        m_impl->outgoing.clear();
        m_impl->closeQueued = false;
    }
    const JUrl u = JUrl::parse(url);
    std::string bad;
    if (!u.valid || (u.scheme != "ws" && u.scheme != "wss")) bad = "not a ws:// or wss:// url: " + url;
    else if (u.secure && !JTcpSocket::tlsAvailable())         bad = "wss:// needs TLS, which this build does not have";
    if (!bad.empty()) {
        JLOGC(kLog, JLogLevel::Warn) << bad;
        JWebSocket* o = this;
        m_impl->post([o, bad] { o->onClosed.emit(kAbnormalClosure, bad); });
        return;
    }
    m_impl->state.store(State::Connecting);
    m_impl->thread = std::thread([impl = m_impl.get(), u, headers, t = m_impl->timeoutMs] { impl->run(u, headers, t); });
}

bool JWebSocket::sendText(const std::string& text) {
    return m_impl->queue(encodeFrame(kText, text));
}

bool JWebSocket::sendBinary(const std::vector<uint8_t>& data) {
    return m_impl->queue(encodeFrame(kBinary, data.data(), data.size()));
}

bool JWebSocket::ping(const std::string& payload) {
    return m_impl->queue(encodeFrame(kPing, payload.substr(0, kMaxControlBytes)));
}

void JWebSocket::close(int code, const std::string& reason) {
    if (m_impl->state.load() == State::Connecting) { m_impl->join(); return; }
    std::lock_guard<std::mutex> lk(m_impl->outMutex);
    State open = State::Open;                 // not if the connection ended a moment ago: it stays Closed
    if (!m_impl->state.compare_exchange_strong(open, State::Closing)) return;
    m_impl->outgoing.push_back({ encodeFrame(kClose, closePayload(code, reason)), code });
    m_impl->closeQueued = true;
}

JWebSocket::State JWebSocket::state() const { return m_impl->state.load(); }
void JWebSocket::setTimeout(int ms)         { m_impl->timeoutMs = std::max(ms, 1); }
int  JWebSocket::timeout() const            { return m_impl->timeoutMs; }

} // inline namespace jf
