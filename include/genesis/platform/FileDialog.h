#pragma once

#include <string>

#ifndef _WIN32
#  include <cstdio>
#else
#  include <windows.h>
#  include <commdlg.h>
#  include <shlobj.h>
#endif

namespace Genesis {

/**
 * Native file-picker dialogs.
 *
 * All methods are static; no instances are created.
 * Returns empty string when the user cancels or on any error.
 *
 * Linux backend:  zenity (must be installed).
 * Win32 backend:  OPENFILENAMEA / SHBrowseForFolderA.
 */
class JFileDialog {
public:
    JFileDialog() = delete;

    // --------------------------------------------------------------- openFile

    static std::string openFile(const std::string& title  = "Open File",
                                const std::string& filter = "")
    {
        (void)filter; // filter used only where natively supported
#ifndef _WIN32
        return runZenity("zenity --file-selection --title='"
                         + escapeTitle(title) + "' 2>/dev/null");
#else
        char buf[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrTitle  = title.c_str();
        ofn.lpstrFilter = filter.empty() ? nullptr : filter.c_str();
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (::GetOpenFileNameA(&ofn))
            return buf;
        return {};
#endif
    }

    // --------------------------------------------------------------- saveFile

    static std::string saveFile(const std::string& title       = "Save File",
                                const std::string& defaultName = "")
    {
#ifndef _WIN32
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title='"
                          + escapeTitle(title) + "'";
        if (!defaultName.empty())
            cmd += " --filename='" + escapeTitle(defaultName) + "'";
        cmd += " 2>/dev/null";
        return runZenity(cmd);
#else
        char buf[MAX_PATH] = {};
        if (!defaultName.empty() && defaultName.size() < MAX_PATH)
            ::memcpy(buf, defaultName.data(), defaultName.size());

        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrTitle  = title.c_str();
        ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (::GetSaveFileNameA(&ofn))
            return buf;
        return {};
#endif
    }

    // ----------------------------------------------------------- openDirectory

    static std::string openDirectory(const std::string& title = "Select Folder")
    {
#ifndef _WIN32
        return runZenity("zenity --file-selection --directory --title='"
                         + escapeTitle(title) + "' 2>/dev/null");
#else
        char path[MAX_PATH] = {};
        BROWSEINFOA bi{};
        bi.lpszTitle = title.c_str();
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = ::SHBrowseForFolderA(&bi);
        if (pidl) {
            ::SHGetPathFromIDListA(pidl, path);
            // Free the PIDL via CoTaskMemFree (no COM init needed for this call).
            IMalloc* pMalloc = nullptr;
            if (SUCCEEDED(::SHGetMalloc(&pMalloc)) && pMalloc) {
                pMalloc->Free(pidl);
                pMalloc->Release();
            }
            return path;
        }
        return {};
#endif
    }

private:
#ifndef _WIN32
    // Run a zenity command, capture stdout, strip trailing newline(s).
    static std::string runZenity(const std::string& cmd)
    {
        FILE* fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return {};

        std::string result;
        char buf[4096];
        while (::fgets(buf, sizeof(buf), fp))
            result += buf;
        ::pclose(fp);

        // Strip trailing newline characters.
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();

        return result;
    }

    // Escape single-quotes inside a zenity --title value.
    // Strategy: close the single-quoted string, emit \', reopen.
    static std::string escapeTitle(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '\'')
                out += "'\\''";   // end quote, literal ', restart quote
            else
                out += c;
        }
        return out;
    }
#endif
};

} // namespace Genesis
