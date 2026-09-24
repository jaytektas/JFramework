// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// JAppUpdater — an application's own updates, start to finish, the same in every application.
//
// Ask GitHub for the newest release (JRelease), offer it, download this system's file together with the
// release's SHA256SUMS, check the one against the other (JSha256), stage it (JSelfInstaller), and install
// it as the application closes — at once, since the person has just said yes; the close is the usual one,
// so unsaved work is still asked about, and if that close is cancelled the update stays staged and goes
// in whenever the application is next closed.
//
//     JAppUpdater updater(win, { "jscope", JSCOPE_VERSION,
//                                "https://api.github.com/repos/jaytektas/jscope/releases/latest",
//                                "JSCOPE_UPDATE_URL" });
//     updater.check(false);              // at startup: only a newer version is worth interrupting for
//     ...Help > Check for updates:       updater.check(true);
//     win.run();
//     updater.installStaged();           // after the main loop has returned
//
// `manual` is a person asking: then every answer is said out loud, including "you are up to date" and
// "could not check". At startup only a newer version is — a laptop with no network in a workshop should
// not be told so every time the application opens.
//
// FOR TESTING BEFORE A RELEASE EXISTS, the environment variable named in the config replaces the releases
// URL with any URL answering the same JSON shape (see JRelease.h for what a release carries).

#include <j/app/JAppWindow.h>
#include <j/app/JProgressDialog.h>
#include <j/core/Dialog.h>
#include <j/core/Log.h>
#include <j/io/HttpClient.h>
#include <j/update/JRelease.h>
#include <j/update/JSelfInstaller.h>
#include <j/update/JSha256.h>

#include <cstdlib>
#include <string>

inline namespace jf {

class JAppUpdater {
public:
    struct JConfig {
        std::string appName;       // as the person knows it: "jscope", "jayecu Studio"
        std::string version;       // this build, "0.1.0"
        std::string releasesApi;   // https://api.github.com/repos/<owner>/<repo>/releases/latest
        std::string overrideEnv;   // environment variable that replaces releasesApi, for testing
    };

    JAppUpdater(JAppWindow& win, JConfig cfg) : m_win(win), m_cfg(std::move(cfg)) {
        // GitHub refuses requests without a User-Agent.
        const std::string agent = m_cfg.appName + "/" + m_cfg.version;
        m_check.setTimeout(15000);
        m_check.setHeader("User-Agent", agent);
        m_check.setHeader("Accept", "application/vnd.github+json");
        m_download.setTimeout(10 * 60 * 1000);   // a whole release, on a slow connection
        m_download.setHeader("User-Agent", agent);
        m_download.onProgress.connect([](int64_t got, int64_t total) {
            if (auto* d = JProgressDialog::active()) d->setProgress(got, total);
        });
    }

    JAppUpdater(const JAppUpdater&)            = delete;
    JAppUpdater& operator=(const JAppUpdater&) = delete;

    std::string releasesUrl() const {
        const char* over = m_cfg.overrideEnv.empty() ? nullptr : std::getenv(m_cfg.overrideEnv.c_str());
        return (over && *over) ? std::string(over) : m_cfg.releasesApi;
    }

    // Is there a newer release? `manual`: a person asked, so every outcome is reported.
    void check(bool manual) {
        if (m_busy || !m_staged.empty()) {
            if (manual) m_win.showStatus(m_busy ? "An update is already downloading"
                                                : "An update is ready \xE2\x80\x94 it installs when you close " + m_cfg.appName,
                                         kStatusMs);
            return;
        }
        if (manual) m_win.showStatus("Checking for updates \xE2\x80\xA6", kCheckingMs);
        m_check.get(releasesUrl(), [this, manual](const JHttpResponse& r) {
            const JRelease rel = JRelease::fromGitHub(r.status, r.text(), r.error, m_cfg.version);
            JLOGC("updates", JLogLevel::Info)
                << "update check: " << releasesUrl() << " -> status " << r.status
                << (r.error.empty() ? "" : " (" + r.error + ")") << ", latest '" << rel.tag << "'";
            switch (rel.kind) {
            case JRelease::JKind::NewerAvailable: offer(rel); break;
            case JRelease::JKind::UpToDate:
                if (manual) m_win.showStatus(m_cfg.appName + " is up to date (" + m_cfg.version + ")", kStatusMs);
                break;
            case JRelease::JKind::NoReleases:
                if (manual) m_win.showStatus("No " + m_cfg.appName + " releases have been published yet", kStatusMs);
                break;
            case JRelease::JKind::Failed:
                if (manual) m_win.showStatus("Could not check for updates: " + rel.error, kStatusMs);
                break;
            }
        });
    }

    // A downloaded, checked update is waiting to be installed.
    bool staged() const { return !m_staged.empty(); }

    // Install the staged update and start the new version. Call after the main loop has returned, when
    // this copy has finished with its files; does nothing when there is nothing staged.
    void installStaged() {
        if (m_staged.empty()) return;
        std::string err;
        if (!JSelfInstaller::installAndRestart(m_staged, err))
            JLOGC("updates", JLogLevel::Error) << m_cfg.appName << " update not installed: " << err;
    }

private:
    static constexpr int kStatusMs   = 6000;
    static constexpr int kCheckingMs = 15000;

    void offer(const JRelease& rel) {
        const std::string head = m_cfg.appName + " " + rel.version + " is available. You have " + m_cfg.version + ".";
        const std::string title = m_cfg.appName + " update available";
        std::string why;
        if (rel.assetUrl.empty()) {
            JDialog::message(title, head + "\n\nThere is no download for this system in that release.");
        } else if (rel.sumsUrl.empty()) {
            JDialog::message(title, head + "\n\nThe release has no checksums, so it cannot be installed safely.");
        } else if (!JSelfInstaller::canInstallHere(why)) {
            JDialog::message(title, head + "\n\nIt cannot be installed automatically: " + why + ".");
        } else {
            const std::string size = rel.assetSize > 0
                ? " (" + std::to_string((rel.assetSize + 1024 * 1024 - 1) / (1024 * 1024)) + " MB)" : "";
            JDialog::confirm(title, head + "\n\nDownload and install it now" + size + "?",
                             [this, rel] { download(rel); }, {}, yesNo());
        }
    }

    // Every update question is a yes/no question, so its buttons say so rather than OK / Cancel.
    static JDialogOptions yesNo() {
        JDialogOptions o; o.okLabel = "Yes"; o.cancelLabel = "No"; return o;
    }

    // A failed download is usually the connection, so it offers to try again rather than just saying it
    // failed. Only a release that is broken in itself — its checksums do not list the file — is reported
    // without a retry, because trying again would get the same answer.
    void fail(const JRelease& rel, const std::string& why, bool canRetry) {
        m_busy = false;
        if (auto* d = JProgressDialog::active()) d->dismiss();
        JLOGC("updates", JLogLevel::Warn) << m_cfg.appName << " update failed: " << why;
        if (canRetry)
            JDialog::confirm(m_cfg.appName + " download failed",
                             why + "\n\nNothing was changed. Try the download again?",
                             [this, rel] { download(rel); }, {}, yesNo());
        else
            JDialog::message(m_cfg.appName + " update failed", why + "\n\nNothing was changed.");
    }

    static std::string failure(const JHttpResponse& r) {
        return r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    }

    void download(const JRelease& rel) {
        m_busy = true;
        m_win.openModal<JProgressDialog>("Downloading " + m_cfg.appName + " " + rel.version, rel.assetName);
        // The checksums first: they are small, and without them the big download could not be trusted.
        m_download.get(rel.sumsUrl, [this, rel](const JHttpResponse& sr) {
            if (!sr.ok()) { fail(rel, "Could not download the release's checksums (" + failure(sr) + ").", true); return; }
            const std::string want = JSha256::sumFor(sr.text(), rel.assetName);
            if (want.empty()) { fail(rel, "The release's checksums do not list " + rel.assetName + ".", false); return; }
            m_download.get(rel.assetUrl, [this, rel, want](const JHttpResponse& ar) {
                if (!ar.ok()) { fail(rel, "The download did not complete (" + failure(ar) + ").", true); return; }
                if (JSha256::hex(ar.body) != want) {
                    fail(rel, "The downloaded file does not match its published checksum, so it was not installed.", true);
                    return;
                }
                std::string staged, err;
                if (!JSelfInstaller::stage(ar.body, rel.assetName, staged, err)) { fail(rel, err + ".", true); return; }
                m_busy = false;
                if (auto* d = JProgressDialog::active()) d->dismiss();
                m_staged = staged;
                JLOGC("updates", JLogLevel::Info) << m_cfg.appName << " " << rel.version << " staged at " << staged;
                // They already said yes, so no second question: close and install now.
                m_win.showStatus("Installing " + m_cfg.appName + " " + rel.version + " \xE2\x80\x94 restarting", kStatusMs);
                m_win.requestClose();
            });
        });
    }

    JAppWindow& m_win;
    JConfig     m_cfg;
    JHttpClient m_check, m_download;
    std::string m_staged;      // installed by installStaged(); "" = nothing to do
    bool        m_busy = false;
};

} // inline namespace jf
