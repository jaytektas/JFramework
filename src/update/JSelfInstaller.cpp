// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/update/JSelfInstaller.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <cerrno>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

inline namespace jf {

static bool writeFile(const std::string& path, const std::vector<uint8_t>& data, std::string& error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write " + path; return false; }
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) { error = "writing " + path + " failed (disk full?)"; return false; }
    return true;
}

#if defined(_WIN32)

bool JSelfInstaller::canInstallHere(std::string&) { return true; }   // the installer decides where it goes

bool JSelfInstaller::stage(const std::vector<uint8_t>& data, const std::string& assetName,
           std::string& stagedPath, std::string& error) {
    char tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH) { error = "no temporary folder"; return false; }
    stagedPath = std::string(tmp) + assetName;
    return writeFile(stagedPath, data, error);
}

bool JSelfInstaller::installAndRestart(const std::string& stagedPath, std::string& error) {
    // /SILENT: progress only, no questions. /SUPPRESSMSGBOXES: nor any message box. /CLOSEAPPLICATIONS:
    // if this application has not quite finished exiting, the installer waits for it rather than failing on
    // a file in use. /NORESTART: never reboot the computer.
    const HINSTANCE h = ShellExecuteA(nullptr, "open", stagedPath.c_str(),
                                      "/SILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /NORESTART",
                                      nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) { error = "could not start the installer"; return false; }
    return true;
}

#else

bool JSelfInstaller::canInstallHere(std::string& why) {
    // Set by the AppImage runtime to the file it was started from. Its absence means this copy was
    // not started from an AppImage, and there is no single file to replace.
    const char* self = std::getenv("APPIMAGE");
    if (!self || !*self) {
        why = "this copy was not started from an AppImage, so it cannot replace itself";
        return false;
    }
    std::string dir = self;
    const size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? "." : (slash == 0 ? "/" : dir.substr(0, slash));
    if (access(dir.c_str(), W_OK) != 0) {
        why = "the folder the AppImage is in (" + dir + ") cannot be written to";
        return false;
    }
    return true;
}

bool JSelfInstaller::stage(const std::vector<uint8_t>& data, const std::string&,
           std::string& stagedPath, std::string& error) {
    std::string why;
    if (!canInstallHere(why)) { error = why; return false; }
    stagedPath = std::string(std::getenv("APPIMAGE")) + ".new";
    if (!writeFile(stagedPath, data, error)) return false;
    if (chmod(stagedPath.c_str(), 0755) != 0) { error = "cannot make " + stagedPath + " executable"; return false; }
    return true;
}

bool JSelfInstaller::installAndRestart(const std::string& stagedPath, std::string& error) {
    const char* self = std::getenv("APPIMAGE");
    if (!self || !*self) { error = "APPIMAGE is not set"; return false; }
    const std::string target = self;
    if (std::rename(stagedPath.c_str(), target.c_str()) != 0) {
        error = "cannot replace " + target + ": " + std::strerror(errno);
        return false;
    }
    // Start the new copy in its own session, so it outlives this one and is not tied to its terminal.
    const pid_t pid = fork();
    if (pid < 0) { error = "cannot start the new version"; return false; }
    if (pid == 0) {
        setsid();
        execl(target.c_str(), target.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}

#endif

} // inline namespace jf
