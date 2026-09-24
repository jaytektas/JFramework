// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/platform/JUdevRule.h>

#include <j/core/MainThreadDispatcher.h>

#if !defined(_WIN32)
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
#endif

inline namespace jf {

JUdevRule::JUdevRule(std::string fileName, std::string contents, std::vector<std::string> markers,
                     std::vector<std::string> retire)
    : m_fileName(std::move(fileName)), m_contents(std::move(contents)),
      m_markers(std::move(markers)), m_retire(std::move(retire)) {}

#if defined(_WIN32)

bool JUdevRule::installed() const { return true; }

void JUdevRule::install(std::function<void(bool, const std::string&)> done) const {
    if (done) done(true, {});
}

#else

bool JUdevRule::installed() const {
    namespace fs = std::filesystem;
    for (const char* dir : { "/etc/udev/rules.d", "/usr/lib/udev/rules.d" }) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (e.path().extension() != ".rules" || name.size() < 2 || name.substr(0, 2) >= "73") continue;
            std::ifstream f(e.path());
            std::stringstream ss; ss << f.rdbuf();
            const std::string text = ss.str();
            bool all = true;
            for (const std::string& m : m_markers) all = all && text.find(m) != std::string::npos;
            if (all) return true;
        }
    }
    return false;
}

void JUdevRule::install(std::function<void(bool, const std::string&)> done) const {
    std::thread([self = *this, done = std::move(done)] {
        bool ok = false;
        std::string error;
        // The rule goes to a private temp file first; only the copy into /etc runs as root.
        char tmpl[] = "/tmp/jf-udev-XXXXXX";
        const int fd = mkstemp(tmpl);
        if (fd < 0) error = "cannot write a temporary file";
        else {
            ok = write(fd, self.m_contents.data(), self.m_contents.size()) == ssize_t(self.m_contents.size());
            close(fd);
            if (!ok) error = "cannot write a temporary file";
        }
        if (ok) {
            // Retired copies go first: they would otherwise sit there looking like the answer.
            std::string script;
            for (const std::string& old : self.m_retire) script += "rm -f /etc/udev/rules.d/" + old + " && ";
            script += std::string("install -m 644 ") + tmpl + " /etc/udev/rules.d/" + self.m_fileName + " && "
                      "udevadm control --reload-rules && "
                      "udevadm trigger --action=change --subsystem-match=usb --subsystem-match=tty";
            const std::string cmd = "pkexec sh -c '" + script + "' 2>&1";
            FILE* p = popen(cmd.c_str(), "r");
            std::string out;
            if (p) { char buf[256]; while (fgets(buf, sizeof buf, p)) out += buf; }
            const int status = p ? pclose(p) : -1;
            const int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
            ok = code == 0 && self.installed();
            // pkexec: 126 = the prompt was dismissed, 127 = not authorised.
            if (!ok) error = (code == 126 || code == 127) ? "the password prompt was cancelled or refused"
                                 : "installing the USB rule failed" + (out.empty() ? std::string() : ": " + out);
        }
        std::remove(tmpl);
        JMainThreadDispatcher::instance().post([done, ok, error] { if (done) done(ok, error); });
    }).detach();
}

#endif

} // inline namespace jf
