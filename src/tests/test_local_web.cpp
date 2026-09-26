// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JLocalWebServer: serves a mounted folder to a browser on 127.0.0.1 and nothing else — not a file
// outside the mount however the path is spelled, not an unmounted prefix, not a non-GET request.

#include <j/io/HttpClient.h>
#include <j/io/JLocalWebServer.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace jf;
namespace fs = std::filesystem;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static void write(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p) << s; }

int main() {
    std::printf("=== local web server ===\n");
    const fs::path root = fs::temp_directory_path() / "jf_local_web_test";
    fs::remove_all(root);
    write(root / "site" / "index.html", "<h1>manual</h1>");
    write(root / "site" / "css" / "a.css", "body{}");
    write(root / "site" / "part" / "index.html", "<p>part</p>");
    write(root / "secret.txt", "do not serve");
    std::error_code ec;
    fs::create_symlink(root / "secret.txt", root / "site" / "link.txt", ec);   // a way out that is not ".."

    JLocalWebServer web;
    web.mount("manual", (root / "site").string());

    // The rule, without a socket.
    CHECK(web.resolve("/manual/index.html") == (fs::weakly_canonical(root / "site" / "index.html")).string());
    CHECK(!web.resolve("/manual/").empty());                    // a folder serves its index.html
    CHECK(!web.resolve("/manual/part").empty());
    CHECK(web.resolve("/manual/../secret.txt").empty());        // climbing out by ".."
    CHECK(web.resolve("/manual/css/../../secret.txt").empty());
    CHECK(web.resolve("/manual/link.txt").empty());             // or out through a symlink
    CHECK(web.resolve("/other/index.html").empty());            // an unmounted prefix
    CHECK(web.resolve("/manual/missing.html").empty());
    CHECK(web.resolve("manual/index.html").empty());            // not a path at all
    CHECK(JLocalWebServer::mimeType("a/b.CSS") == "text/css; charset=utf-8");
    CHECK(JLocalWebServer::mimeType("x.svg") == "image/svg+xml");

    // And over the wire.
    std::string why;
    CHECK(web.start(why));
    CHECK(web.port() > 0);
    CHECK(web.url("manual/index.html").rfind("http://127.0.0.1:", 0) == 0);
    const JHttpResponse ok = JHttpClient::getSync(web.url("manual/index.html"), 3000);
    CHECK(ok.status == 200 && ok.text() == "<h1>manual</h1>");
    CHECK(ok.header("Content-Type").rfind("text/html", 0) == 0);
    const JHttpResponse css = JHttpClient::getSync(web.url("manual/css/a.css"), 3000);
    CHECK(css.status == 200 && css.header("Content-Type").rfind("text/css", 0) == 0);
    CHECK(JHttpClient::getSync(web.url("manual/nope.html"), 3000).status == 404);
    CHECK(JHttpClient::getSync(web.url("manual/%2e%2e/secret.txt"), 3000).status == 404);   // encoded ".."
    CHECK(JHttpClient::getSync(web.url("manual/index.html?x=1#top"), 3000).status == 200);  // query ignored
    web.stop();
    CHECK(!web.running());

    fs::remove_all(root);
    std::printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
