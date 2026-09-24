// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#include <j/platform/JDesktop.h>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

inline namespace jf {

#if defined(_WIN32)

bool JDesktop::openUrl(const std::string& urlOrPath) {
    // UTF-8 in, UTF-16 to the shell, so a path with non-ASCII in it reaches the right file.
    const int n = MultiByteToWideChar(CP_UTF8, 0, urlOrPath.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, urlOrPath.c_str(), -1, w.data(), n);
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
}

#else

bool JDesktop::openUrl(const std::string& urlOrPath) {
    // Double fork: the grandchild runs xdg-open in its own session and is re-parented to init, so the
    // application never has a zombie to reap and closing it does not take the browser down.
    //
    // Whether the exec HAPPENED comes back through a close-on-exec pipe: a successful exec closes it
    // (the parent reads end-of-file), a failed one -- no xdg-open on this system -- writes a byte first.
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        close(fds[0]);
        setsid();
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", urlOrPath.c_str(), static_cast<char*>(nullptr));
            const char failed = 1;
            (void)!write(fds[1], &failed, 1);
            _exit(127);
        }
        _exit(0);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char byte = 0;
    ssize_t got;
    do { got = read(fds[0], &byte, 1); } while (got < 0 && errno == EINTR);
    close(fds[0]);
    return got == 0;   // end-of-file: the exec succeeded and closed the pipe
}

#endif

} // inline namespace jf
