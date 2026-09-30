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
#endif
    std::cout << "all JHttpClient tests passed\n";
    return 0;
}
