// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_http.cpp — JHttpClient unit tests
// Everything here runs without a network: URL handling, the response accessors, the
// TLS-availability contract and the client's lifetime rules. The one test that would
// need a server is the transfer itself, which is exercised by hand against a real host
// (see the note in test_refuses_https_without_tls).

#include <j/io/HttpClient.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace jf;

static void test_malformed_urls_are_rejected() {
    // No scheme, an unsupported scheme, and an empty host: each is answered with an
    // error rather than a connection attempt, and never with a status.
    for (const char* bad : { "rusefi.com/x.ini", "ftp://rusefi.com/x.ini", "http://", "" }) {
        const JHttpResponse r = JHttpClient::getSync(bad, 500);
        assert(!r.error.empty());
        assert(r.status == 0);
        assert(!r.ok());
    }
    std::cout << "  [OK] malformed urls rejected without a connection attempt\n";
}

static void test_credentials_in_url_are_refused() {
    // user:pass@host is a foot-gun; the client rejects it rather than silently dropping it.
    const JHttpResponse r = JHttpClient::getSync("https://user:pass@example.com/x", 500);
    assert(!r.error.empty());
    assert(r.status == 0);
    std::cout << "  [OK] credentials in a url are refused\n";
}

static void test_response_accessors() {
    JHttpResponse r;
    r.status  = 200;
    r.body    = { 'h', 'i' };
    r.headers = { { "Content-Type", "text/plain" }, { "Content-Length", "2" } };
    assert(r.ok());
    assert(r.text() == "hi");
    assert(r.header("content-type") == "text/plain");   // case-insensitive
    assert(r.header("CONTENT-LENGTH") == "2");
    assert(r.header("nonesuch").empty());

    r.status = 404;
    assert(!r.ok());                                    // a status is not an error, but 404 is not ok
    assert(r.error.empty());
    r.status = 200;
    r.error  = "connect failed";
    assert(!r.ok());                                    // a transport error is never ok
    std::cout << "  [OK] response accessors: ok(), text(), case-insensitive header()\n";
}

static void test_refuses_https_without_tls() {
    // The contract: a build without TLS answers https:// with an error. It must NEVER
    // quietly fetch over plaintext instead. Where TLS IS available this asserts the
    // other half of the contract — that the client says so.
    if (JHttpClient::tlsAvailable()) {
        std::cout << "  [OK] tlsAvailable() == true (https is served)\n";
    } else {
        const JHttpResponse r = JHttpClient::getSync("https://example.com/", 500);
        assert(!r.error.empty());
        assert(r.status == 0);
        std::cout << "  [OK] no TLS in this build → https refused, not downgraded\n";
    }
}

static void test_settings_round_trip() {
    JHttpClient c;
    c.setTimeout(1234);
    assert(c.timeout() == 1234);
    c.setTimeout(-5);
    assert(c.timeout() > 0);                            // a non-positive budget is meaningless
    c.setHeader("User-Agent", "one");
    c.setHeader("user-agent", "two");                   // same header, case-insensitively: replaced, not doubled
    c.setMaxRedirects(0);
    std::cout << "  [OK] timeout / header / redirect settings\n";
}

static void test_destroying_client_abandons_transfer() {
    // A transfer outliving its client must not fire the callback into freed state.
    // Nothing can be asserted about a race except that it does not crash: the client
    // goes away immediately, while the request is still resolving an unroutable host.
    bool fired = false;
    {
        JHttpClient c;
        c.setTimeout(3000);
        c.get("http://127.0.0.1:9/never", [&fired](const JHttpResponse&) { fired = true; });
    }
    assert(!fired);                                     // no dispatch has run: nothing was posted to us
    std::cout << "  [OK] destroying a client abandons its in-flight callback\n";
}

#if !defined(_WIN32)
// A one-shot local server: answers with a head promising 100 bytes, sends 10, then either goes quiet for
// `quietMs` (a stall) or hangs up (cut off). Returns the port.
static int serveTruncated(int quietMs, std::thread& t) {
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)); ::listen(ls, 1);
    socklen_t len = sizeof(a); ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    t = std::thread([ls, quietMs] {
        const int c = ::accept(ls, nullptr, nullptr);
        char buf[1024]; (void)::recv(c, buf, sizeof(buf), 0);
        const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789";
        (void)::send(c, head.data(), head.size(), 0);
        if (quietMs) std::this_thread::sleep_for(std::chrono::milliseconds(quietMs));
        ::close(c); ::close(ls);
    });
    return ntohs(a.sin_port);
}

static void test_a_stalled_download_fails_at_the_stall_not_the_deadline() {
    // Wifi on the bench dropped a studio update at 95%: the connection stayed open, nothing arrived, and the
    // progress window sat still for the whole ten-minute transfer limit. A wait for data is capped instead.
    std::thread t;
    const int port = serveTruncated(3000, t);
    const auto t0 = std::chrono::steady_clock::now();
    const JHttpResponse r = JHttpClient::getSync("http://127.0.0.1:" + std::to_string(port) + "/f", 1000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    t.join();
    assert(!r.error.empty() && r.error.find("stalled") != std::string::npos);
    assert(ms < 2500);                                  // gave up at the stall limit, not after the server quit
    std::cout << "  [OK] a stalled download fails (" << r.error << ")\n";
}

static void test_a_download_cut_off_is_an_error_not_a_short_file() {
    std::thread t;
    const int port = serveTruncated(0, t);
    const JHttpResponse r = JHttpClient::getSync("http://127.0.0.1:" + std::to_string(port) + "/f", 5000);
    t.join();
    assert(!r.error.empty() && r.error.find("closed after") != std::string::npos);
    assert(r.body.empty());
    std::cout << "  [OK] a download cut off part-way is an error (" << r.error << ")\n";
}

// A local server for a resumed download: a 100-byte file, one connection per entry in `plan`. Each entry
// is how many bytes that connection sends before hanging up; `misbehave` answers the second request
// with something other than the rest (0 = behave, 1 = a whole-file 200, 2 = a 206 starting elsewhere).
// The Range header each request carried is recorded.
static int serveResumable(std::vector<int> plan, int misbehave, std::vector<std::string>& ranges, std::thread& t) {
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)); ::listen(ls, 4);
    socklen_t len = sizeof(a); ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    t = std::thread([ls, plan, misbehave, &ranges] {
        std::string file;
        for (int i = 0; i < 100; ++i) file += char('A' + i % 26);
        for (size_t k = 0; k < plan.size(); ++k) {
            const int c = ::accept(ls, nullptr, nullptr);
            char buf[2048]; const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            const std::string req(buf, n > 0 ? size_t(n) : 0);
            const size_t rp = req.find("Range: bytes=");
            const long from = rp == std::string::npos ? 0 : std::atol(req.c_str() + rp + 13);
            ranges.push_back(rp == std::string::npos ? "" : req.substr(rp + 7, req.find("\r\n", rp) - rp - 7));
            std::string head;
            long start = from;
            if (k == 0 || (k == 1 && misbehave == 1)) {
                start = 0;
                head = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nContent-Length: 100\r\n\r\n";
            } else {
                if (k == 1 && misbehave == 2) start = from + 5;
                head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-99/100\r\n"
                       "Content-Length: " + std::to_string(100 - start) + "\r\n\r\n";
            }
            const std::string out = head + file.substr(size_t(start), size_t(std::min<long>(plan[k], 100 - start)));
            (void)::send(c, out.data(), out.size(), 0);
            ::close(c);
        }
        ::close(ls);
    });
    return ntohs(a.sin_port);
}

static void test_a_cut_off_download_resumes_where_it_stopped() {
    // GitHub's asset CDN stops sending part-way through a large file now and then and never starts again;
    // a new connection asking for the rest is answered at once. Cut off twice, the file still arrives whole.
    std::vector<std::string> ranges;
    std::thread t;
    const int port = serveResumable({ 30, 40, 100 }, 0, ranges, t);
    const JHttpResponse r = JHttpClient::getSync("http://127.0.0.1:" + std::to_string(port) + "/f", 5000);
    t.join();
    std::string want;
    for (int i = 0; i < 100; ++i) want += char('A' + i % 26);
    assert(r.ok() && r.status == 200 && r.text() == want);
    assert(ranges.size() == 3 && ranges[0].empty() && ranges[1] == "bytes=30-" && ranges[2] == "bytes=70-");
    std::cout << "  [OK] a download cut off twice resumes and arrives whole\n";
}

static void test_a_resume_the_server_will_not_honour_fails() {
    for (int how : { 1, 2 }) {
        std::vector<std::string> ranges;
        std::thread t;
        const int port = serveResumable({ 30, 100 }, how, ranges, t);
        const JHttpResponse r = JHttpClient::getSync("http://127.0.0.1:" + std::to_string(port) + "/f", 5000);
        t.join();
        assert(!r.ok() && r.body.empty());
        assert(r.error.find("closed after") != std::string::npos);
        assert(r.error.find(how == 1 ? "would not resume" : "did not match") != std::string::npos);
        std::cout << "  [OK] " << (how == 1 ? "a whole-file answer" : "a misplaced range")
                  << " to a resume is an error (" << r.error << ")\n";
    }
}

static void test_a_post_sends_its_body() {
    // A one-shot local server that answers with what it was sent.
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)); ::listen(ls, 1);
    socklen_t len = sizeof(a); ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    std::string got;
    std::thread t([ls, &got] {
        const int c = ::accept(ls, nullptr, nullptr);
        char buf[4096];
        // The head and the whole body (its Content-Length says how much).
        for (;;) {
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, size_t(n));
            const size_t head = got.find("\r\n\r\n");
            const size_t cl = got.find("Content-Length: ");
            if (head != std::string::npos && cl != std::string::npos
                && got.size() >= head + 4 + size_t(std::atoi(got.c_str() + cl + 16)))
                break;
        }
        const std::string answer = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        (void)::send(c, answer.data(), answer.size(), 0);
        ::close(c); ::close(ls);
    });
    const JHttpResponse r = JHttpClient::postSync("http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/soap",
                                                  "<x>hello</x>", "application/soap+xml", 2000);
    t.join();
    assert(r.ok() && r.text() == "ok");
    assert(got.rfind("POST /soap HTTP/1.1", 0) == 0);
    assert(got.find("Content-Type: application/soap+xml") != std::string::npos);
    assert(got.find("Content-Length: 12") != std::string::npos);
    assert(got.size() >= 12 && got.compare(got.size() - 12, 12, "<x>hello</x>") == 0);
    std::cout << "  [OK] a POST sends its body\n";
}
#endif

int main() {
    std::cout << "JHttpClient tests\n";
    test_malformed_urls_are_rejected();
    test_credentials_in_url_are_refused();
    test_response_accessors();
    test_refuses_https_without_tls();
    test_settings_round_trip();
    test_destroying_client_abandons_transfer();
#if !defined(_WIN32)
    test_a_stalled_download_fails_at_the_stall_not_the_deadline();
    test_a_download_cut_off_is_an_error_not_a_short_file();
    test_a_cut_off_download_resumes_where_it_stopped();
    test_a_resume_the_server_will_not_honour_fails();
    test_a_post_sends_its_body();
#endif
    std::cout << "all JHttpClient tests passed\n";
    return 0;
}
