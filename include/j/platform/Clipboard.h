#pragma once

#include <string>

inline namespace jf {

/**
 * Platform-agnostic text clipboard.
 *
 * The public surface carries no platform types — the Win32 / X11 backend lives
 * in Clipboard.cpp behind this interface (CLAUDE.md platform boundary).
 * All methods are static; no instances are created. Returns empty string on any
 * failure — never throws.
 *
 * NOTE (self-contained follow-up): the Linux backend currently shells out to
 * xclip/xsel. Replacing that with a native xcb selection owner (served from the
 * platform window's event loop) is tracked but not yet done.
 */
class JClipboard {
public:
    JClipboard() = delete;

    static void setText(const std::string& text);
    static std::string getText();
};

} // inline namespace jf
