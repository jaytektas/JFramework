#pragma once

#include <j/core/Signal.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Cross-platform HTTP/HTTPS client. The public interface carries no platform or
// TLS types — the POSIX socket + OpenSSL / Win32 Schannel backend lives entirely
// in HttpClient.cpp behind an opaque Impl (pimpl). get() runs the transfer on its
// own thread and delivers the result on the main thread via JMainThreadDispatcher,
// so a callback may touch widgets directly; getSync() is the same transfer for
// callers that are already off the main thread.

inline namespace jf {

// ---- One header of a request or a response ---------------------------------
struct JHttpHeader {
    std::string name;
    std::string value;
};

// ---- What a transfer produced ----------------------------------------------
// A response with a status is a response, whatever the status: 404 is a server
// speaking, not a failure of the client. `error` is set only when no HTTP
// exchange happened at all — DNS, connect, TLS, timeout — and then `status` is 0.
struct JHttpResponse {
    int                       status{0};
    std::vector<uint8_t>      body;
    std::vector<JHttpHeader>  headers;
    std::string               error;
    std::string               finalUrl;   // after redirects; == the request url when none were followed

    bool ok()   const { return error.empty() && status >= 200 && status < 300; }
    std::string text() const { return std::string(body.begin(), body.end()); }
    std::string header(const std::string& name) const;   // case-insensitive; "" when absent
};

class JHttpClient {
public:
    JHttpClient();
    ~JHttpClient();

    JHttpClient(const JHttpClient&)            = delete;
    JHttpClient& operator=(const JHttpClient&) = delete;

    // GET, off-thread. `done` fires on the MAIN thread. Destroying the client
    // before a transfer completes abandons the callback rather than firing it
    // into freed state.
    void get(const std::string& url, std::function<void(const JHttpResponse&)> done);

    // GET, blocking, on the calling thread. Safe to call from a JWorkerThread.
    static JHttpResponse getSync(const std::string& url, int timeoutMs = 15000,
                                 const std::vector<JHttpHeader>& headers = {});

    // Total transfer budget for get(), connect through last byte.
    void setTimeout(int ms);
    int  timeout() const;

    // Sent with every request this client makes.
    void setHeader(const std::string& name, const std::string& value);

    // Redirects to follow before giving up (0 = return the 3xx as-is).
    void setMaxRedirects(int n);

    // True when the build has TLS. An https:// request without it is answered
    // with an error rather than a silent downgrade to plaintext.
    static bool tlsAvailable();

    // Progress of the running transfer: bytes received, and the total the server
    // declared (0 when it did not). Fires on the main thread.
    jf::JSignal<int64_t, int64_t> onProgress;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // inline namespace jf
