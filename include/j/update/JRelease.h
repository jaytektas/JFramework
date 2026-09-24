// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JRelease — what a GitHub releases API answer says: is there a newer version than this one, and which
// of its files is for this computer?
//
// WHAT A RELEASE MUST CARRY, one file per system plus the checksums of them all:
//     <name>-<version>-setup.exe           Windows (an installer that installs over the running copy)
//     <name>-<version>-x86_64.AppImage     Linux
//     SHA256SUMS                           `sha256sum` output for the files above
// Other files may sit beside them (a .deb, a zip); they are simply not this system's update. A release
// without SHA256SUMS is offered but never installed: the checksum is what proves the file that arrived
// is the file that was published.
//
// The releases API rather than the web page: the page is HTML for people, the API is JSON with the tag
// in a named field, and /releases/latest already leaves out drafts and pre-releases, which are exactly
// what an ordinary user should not be offered.

#include <j/config/Json.h>
#include <j/update/JVersion.h>

#include <string>

inline namespace jf {

struct JRelease {
    enum class JKind {
        UpToDate,      // the newest release is this version (or older)
        NewerAvailable,
        NoReleases,    // the repository answered 404: not published yet, or no release made
        Failed         // no answer, or an answer that is not a release
    } kind = JKind::Failed;
    std::string tag;       // the release's tag, as written ("v0.2.0")
    std::string version;   // the same as a version ("0.2.0") — what the person is shown
    std::string url;       // the release's page
    std::string error;     // why, when Failed
    // This system's file, when the release has one. Empty assetUrl = nothing for this system (the
    // release is still newer; there is just nothing here to install).
    std::string assetName, assetUrl;
    long long   assetSize = 0;
    std::string sumsUrl;   // the release's SHA256SUMS; empty = the release cannot be verified

    // The ending that marks this system's file among a release's assets.
    static const char* platformAssetSuffix() {
#if defined(_WIN32)
        return "-setup.exe";
#else
        return "-x86_64.AppImage";
#endif
    }

    // Read the HTTP answer. Kept apart from the transfer so it can be tested with a string.
    static JRelease fromGitHub(int status, const std::string& body, const std::string& transportError,
                               const std::string& currentVersion) {
        JRelease r;
        if (!transportError.empty()) { r.error = transportError; return r; }
        if (status == 404) { r.kind = JKind::NoReleases; return r; }
        if (status < 200 || status >= 300) { r.error = "HTTP " + std::to_string(status); return r; }

        const auto parsed = JJson::tryParse(body);
        if (!parsed || !parsed->isObject()) { r.error = "the answer was not a release"; return r; }
        // CONST, deliberately: on a non-const JJson, ["assets"] inserts a null for a missing key and
        // .arr() then throws — a release with no files attached would have crashed the application.
        const JJson& doc = *parsed;
        r.tag = doc["tag_name"].str();
        r.url = doc["html_url"].str();
        const JVersion latest = JVersion::parse(r.tag);
        if (!latest.valid) { r.error = "the release tag '" + r.tag + "' is not a version"; return r; }
        r.version = latest.text();

        const std::string suffix = platformAssetSuffix();
        for (const auto& a : doc["assets"].arr()) {
            const std::string name = a["name"].str();
            const std::string url  = a["browser_download_url"].str();
            if (name == "SHA256SUMS") { r.sumsUrl = url; continue; }
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                r.assetName = name; r.assetUrl = url; r.assetSize = a["size"].number<long long>(0);
            }
        }

        const JVersion mine = JVersion::parse(currentVersion);
        r.kind = (mine.valid && !latest.isNewerThan(mine)) ? JKind::UpToDate : JKind::NewerAvailable;
        return r;
    }
};

} // inline namespace jf
