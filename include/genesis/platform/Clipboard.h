#pragma once

#include <string>

#ifndef _WIN32
#  include <cstdio>
#else
#  include <windows.h>
#endif

namespace Genesis {

/**
 * Platform-agnostic text clipboard.
 *
 * All methods are static; no instances are created.
 * Returns empty string on any failure — never throws.
 */
class JClipboard {
public:
    JClipboard() = delete;

    // ------------------------------------------------------------------ set --

    static void setText(const std::string& text)
    {
#ifndef _WIN32
        // Try xclip first; fall back to xsel if popen returns null.
        FILE* fp = ::popen("xclip -selection clipboard", "w");
        if (!fp)
            fp = ::popen("xsel --clipboard --input", "w");
        if (!fp)
            return;
        ::fwrite(text.data(), 1, text.size(), fp);
        ::pclose(fp);
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

    // ------------------------------------------------------------------ get --

    static std::string getText()
    {
#ifndef _WIN32
        // Single shell command: try xclip, fall back to xsel via || operator.
        FILE* fp = ::popen(
            "xclip -selection clipboard -o 2>/dev/null "
            "|| xsel --clipboard --output 2>/dev/null",
            "r");
        if (!fp)
            return {};

        std::string result;
        char buf[4096];
        while (::fgets(buf, sizeof(buf), fp))
            result += buf;
        ::pclose(fp);
        return result;
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
};

} // namespace Genesis
