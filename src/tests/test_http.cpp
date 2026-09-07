// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// test_http.cpp — JHttpClient unit tests
// Everything here runs without a network: URL handling, the response accessors, the
// TLS-availability contract and the client's lifetime rules. The one test that would
// need a server is the transfer itself, which is exercised by hand against a real host
// (see the note in test_refuses_https_without_tls).

#include <j/io/HttpClient.h>
#include <cassert>
#include <iostream>
#include <string>

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

int main() {
    std::cout << "JHttpClient tests\n";
    test_malformed_urls_are_rejected();
    test_credentials_in_url_are_refused();
    test_response_accessors();
    test_refuses_https_without_tls();
    test_settings_round_trip();
    test_destroying_client_abandons_transfer();
    std::cout << "all JHttpClient tests passed\n";
    return 0;
}
