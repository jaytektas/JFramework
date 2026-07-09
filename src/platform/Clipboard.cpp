#include <j/platform/Clipboard.h>

// Platform backend for JClipboard — confined to this translation unit so the
// public header leaks no windows.h / HGLOBAL type (CLAUDE.md platform boundary).
// Linux routes through the installed native hook (LinuxPlatformWindow's xcb
// CLIPBOARD selection) — no xclip/xsel shell-out; falls back to an in-process
// store when headless. Windows uses the Win32 clipboard API directly.
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <cstring>
#endif

inline namespace jf {

#ifndef _WIN32
static std::string& _clipboardFallback() { static std::string s; return s; }
#endif

void JClipboard::setText(const std::string& text) {
#ifndef _WIN32
    if (s_setHook) s_setHook(text);
    else           _clipboardFallback() = text;
#else
    if (!::OpenClipboard(nullptr))
        return;
    ::EmptyClipboard();

    // Allocate global memory for the text (include null terminator).
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hMem) {
        ::CloseClipboard();
        return;
    }
    char* dst = static_cast<char*>(::GlobalLock(hMem));
    if (dst) {
        ::memcpy(dst, text.data(), text.size());
        dst[text.size()] = '\0';
        ::GlobalUnlock(hMem);
    }
    ::SetClipboardData(CF_TEXT, hMem);
    ::CloseClipboard();
    // hMem is owned by the clipboard after SetClipboardData — do not free.
#endif
}

std::string JClipboard::getText() {
#ifndef _WIN32
    return s_getHook ? s_getHook() : _clipboardFallback();
#else
    if (!::OpenClipboard(nullptr))
        return {};

    std::string result;
    HANDLE hData = ::GetClipboardData(CF_TEXT);
    if (hData) {
        const char* src = static_cast<const char*>(::GlobalLock(hData));
        if (src)
            result = src;
        ::GlobalUnlock(hData);
    }
    ::CloseClipboard();
    return result;
#endif
}

} // inline namespace jf
