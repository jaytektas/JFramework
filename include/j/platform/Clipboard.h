#pragma once

#include <string>
#include <functional>

inline namespace jf {

/**
 * Platform-agnostic text clipboard.
 *
 * The public surface carries no platform types. On Linux the framework installs
 * a native backend (LinuxPlatformWindow's xcb CLIPBOARD selection) via the hooks
 * below — no xclip/xsel shell-out. On Windows the Win32 clipboard API is used
 * directly (Clipboard.cpp). When no backend is installed (headless / unit tests)
 * both fall back to ONE in-process store, so copy/paste still round-trips.
 *
 * All methods are static; returns empty string on failure — never throws.
 */
class JClipboard {
public:
    JClipboard() = delete;

    static void setText(const std::string& text);
    static std::string getText();

    // Platform backend, installed by the app window (e.g. JAppWindow → the OS window).
    static inline std::function<void(const std::string&)> s_setHook;
    static inline std::function<std::string()>            s_getHook;
};

} // inline namespace jf
