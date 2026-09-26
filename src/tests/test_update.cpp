// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// An application's own update, short of the network (JRelease, JVersion, JSha256, JSelfInstaller). The
// transfer is JHttpClient's; what can go wrong HERE is the reading of it — a tag that is not compared as a number ("0.10.0" older than "0.9.0"), a 404
// before anything is published reported as an error, this very version reported as an update, the wrong
// system's file picked, a bad checksum passed — and, on Linux, the AppImage swap itself.
//
//   ./build/test_update
//
#include <j/update/JRelease.h>
#include <j/update/JSelfInstaller.h>
#include <j/update/JSha256.h>
#include <j/update/JVersion.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[update] %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

using jf::JRelease;
using K = JRelease::JKind;

static std::string release(const char* tag) {
    return std::string("{\"tag_name\":\"") + tag + "\",\"html_url\":\"https://example/rel\"}";
}

static void testParse() {
    const auto a = jf::JVersion::parse("v1.2.3");
    check(a.valid && a.major == 1 && a.minor == 2 && a.patch == 3, "v1.2.3 parses");
    const auto b = jf::JVersion::parse("0.4");
    check(b.valid && b.major == 0 && b.minor == 4 && b.patch == 0, "0.4 parses as 0.4.0");
    const auto c = jf::JVersion::parse("1.0.0-rc1+build.7");
    check(c.valid && c.major == 1 && c.patch == 0 && c.pre == "rc1" && c.text() == "1.0.0-rc1",
          "a pre-release is kept, build metadata after + is dropped");
    check(!jf::JVersion::parse("latest").valid, "a tag with no number is not a version");
    check(!jf::JVersion::parse("").valid, "an empty tag is not a version");
}

static void testCompare() {
    check(jf::JVersion::parse("0.10.0").isNewerThan(jf::JVersion::parse("0.9.0")), "0.10.0 is newer than 0.9.0 (numbers, not text)");
    check(jf::JVersion::parse("1.0.0").isNewerThan(jf::JVersion::parse("0.99.99")), "a major bump wins");
    check(!jf::JVersion::parse("0.1.0").isNewerThan(jf::JVersion::parse("0.1.0")), "the same version is not newer");
    check(!jf::JVersion::parse("0.1.0").isNewerThan(jf::JVersion::parse("0.2.0")), "an older release is not newer");
    auto newer = [](const char* a, const char* b) { return jf::JVersion::parse(a).isNewerThan(jf::JVersion::parse(b)); };
    check(newer("0.5.0", "0.5.0-beta.1") && !newer("0.5.0-beta.1", "0.5.0"), "a release is newer than its own beta");
    check(newer("0.5.0-beta.1", "0.4.9"), "a beta is newer than the release before it");
    check(newer("0.5.0-beta.10", "0.5.0-beta.2"), "beta.10 is newer than beta.2 (numbers, not text)");
    check(newer("0.5.0-rc.1", "0.5.0-beta.9"), "rc is newer than beta");
    check(newer("0.5.0-beta.1", "0.5.0-beta") , "more parts is newer when the rest is equal");
    check(newer("0.5.0-beta", "0.5.0-1"), "a word ranks above a number");
    check(!newer("0.5.0-beta.1", "0.5.0-beta.1+other"), "build metadata does not make a version newer");
}

static void testInterpret() {
    check(JRelease::fromGitHub(200, release("v0.2.0"), "", "0.1.0").kind == K::NewerAvailable,
          "a newer release is offered");
    check(JRelease::fromGitHub(200, release("v0.1.0"), "", "0.1.0").kind == K::UpToDate,
          "this version is up to date");
    check(JRelease::fromGitHub(200, release("v0.0.9"), "", "0.1.0").kind == K::UpToDate,
          "a build newer than the release is up to date");
    const JRelease r = JRelease::fromGitHub(200, release("v0.2.0"), "", "0.1.0");
    check(r.tag == "v0.2.0" && r.url == "https://example/rel", "the tag and page come back as given");
    check(r.version == "0.2.0", "the version shown drops the tag's v, to match how an application writes its own");
    check(JRelease::fromGitHub(404, "{\"message\":\"Not Found\"}", "", "0.1.0").kind == K::NoReleases,
          "404 (repo not published yet) is 'no releases', not an error");
    check(JRelease::fromGitHub(0, "", "could not resolve host", "0.1.0").kind == K::Failed,
          "no connection is a failure");
    check(JRelease::fromGitHub(403, "rate limited", "", "0.1.0").kind == K::Failed,
          "403 (rate limit) is a failure");
    check(JRelease::fromGitHub(200, "<html>", "", "0.1.0").kind == K::Failed,
          "a page that is not JSON is a failure");
    check(JRelease::fromGitHub(200, release("nightly"), "", "0.1.0").kind == K::Failed,
          "a tag that is not a version is a failure");
}

// FIPS 180-4 test vectors, including the lengths either side of the one-block/two-block padding edge.
static void testSha256() {
    check(jf::JSha256::hex(std::string("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
    check(jf::JSha256::hex(std::string("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of 'abc'");
    check(jf::JSha256::hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 of the 56-byte vector (two padding blocks)");
    check(jf::JSha256::hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million 'a'");
}

static void testAssets() {
    const std::string body =
        "{\"tag_name\":\"v0.2.0\",\"assets\":["
        "{\"name\":\"app-0.2.0-setup.exe\",\"size\":10,\"browser_download_url\":\"https://x/win\"},"
        "{\"name\":\"app-0.2.0-x86_64.AppImage\",\"size\":20,\"browser_download_url\":\"https://x/lin\"},"
        "{\"name\":\"SHA256SUMS\",\"size\":5,\"browser_download_url\":\"https://x/sums\"}]}";
    const JRelease r = JRelease::fromGitHub(200, body, "", "0.1.0");
#if defined(_WIN32)
    check(r.assetUrl == "https://x/win" && r.assetSize == 10, "Windows picks the setup.exe");
#else
    check(r.assetUrl == "https://x/lin" && r.assetSize == 20, "Linux picks the AppImage");
#endif
    check(r.sumsUrl == "https://x/sums", "the SHA256SUMS asset is found");
    const JRelease none = JRelease::fromGitHub(200, release("v0.2.0"), "", "0.1.0");
    check(none.assetUrl.empty() && none.sumsUrl.empty(), "a release with no files offers nothing to install");

    const std::string h(64, 'a'), H(64, 'B');
    const std::string sums = h + "  app-0.2.0-x86_64.AppImage\n" + H + " *app-0.2.0-setup.exe\r\n";
    check(jf::JSha256::sumFor(sums, "app-0.2.0-x86_64.AppImage") == h, "sumFor: text-mode line");
    check(jf::JSha256::sumFor(sums, "app-0.2.0-setup.exe") == std::string(64, 'b'), "sumFor: binary-mode line, CRLF, upper case");
    check(jf::JSha256::sumFor(sums, "app-0.2.0").empty(), "sumFor: a name that is only a prefix is not listed");
}

#if !defined(_WIN32)
// The Linux swap for real, on files in a scratch folder: stage beside the "running" AppImage, rename
// over it, start it. The "new AppImage" is a script that leaves a mark, so starting it can be seen.
static void testLinuxSwap() {
    char tmpl[] = "/tmp/jf_update_XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string app = dir + "/app.AppImage", mark = dir + "/started";
    { std::ofstream(app) << "old"; }
    setenv("APPIMAGE", app.c_str(), 1);
    std::string why;
    check(jf::JSelfInstaller::canInstallHere(why), "an AppImage in a writable folder can update itself");
    const std::string script = "#!/bin/sh\ntouch " + mark + "\n";
    std::string staged, err;
    check(jf::JSelfInstaller::stage(std::vector<uint8_t>(script.begin(), script.end()), "x", staged, err) &&
          staged == app + ".new", "stage writes <AppImage>.new");
    struct stat st{};
    check(stat(staged.c_str(), &st) == 0 && (st.st_mode & S_IXUSR), "the staged file is executable");
    check(jf::JSelfInstaller::installAndRestart(staged, err), "install succeeds");
    std::ifstream f(app); std::string first; std::getline(f, first);
    check(first == "#!/bin/sh", "the AppImage was replaced by the new one");
    bool started = false;
    for (int i = 0; i < 50 && !started; ++i) { started = access(mark.c_str(), F_OK) == 0; if (!started) usleep(100000); }
    check(started, "the new AppImage was started");
    unsetenv("APPIMAGE");
    check(!jf::JSelfInstaller::canInstallHere(why), "without APPIMAGE the application says it cannot update itself");
    std::filesystem::remove_all(dir);
}
#endif

int main() {
    testParse();
    testCompare();
    testInterpret();
    testSha256();
    testAssets();
#if !defined(_WIN32)
    testLinuxSwap();
#endif
    std::printf("\n[update] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
