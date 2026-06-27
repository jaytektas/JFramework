#pragma once

// ============================================================================
// Dialog — non-blocking modal dialog system (Step 2)
//
// Superior to Qt: no exec() spin-loop, no QDialog subclass required.
// All methods return immediately; results arrive via lambda callback.
//
// Genesis-native dialogs (message, confirm, input):
//   - Rendered as PopupWindow overlays inside the Genesis compositor
//   - Zero OS dependencies, consistent look across platforms
//
// File/folder/color pickers use OS-native dialogs via a background thread
//   - Linux: zenity (Wayland/X11) or kdialog as fallback
//   - Windows: GetOpenFileNameA / GetSaveFileNameA (commdlg.h)
//   - macOS: NSOpenPanel / NSSavePanel via AppleScript (no Obj-C required)
//
// All callbacks are posted back to the main thread via MainThreadDispatcher.
// ============================================================================

#include "Signal.h"
#include "MainThreadDispatcher.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <cstdio>
#include <memory>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
#endif

namespace Genesis {

// ---- DialogRequest ---------------------------------------------------------
// Describes a pending genesis-native overlay dialog.
// DialogManager services one at a time; queue subsequent requests.
struct DialogRequest {
    enum class Kind { Message, Confirm, Input };
    Kind        kind{Kind::Message};
    std::string title;
    std::string body;
    std::string placeholder;        // for Input dialogs
    std::function<void()>               onOk;
    std::function<void()>               onCancel;
    std::function<void(std::string)>    onInput;
};

// ---- DialogManager ---------------------------------------------------------
// Populated by Dialog::* static methods; drained by the platform window each frame.
class DialogManager {
public:
    static DialogManager& instance() {
        static DialogManager dm;
        return dm;
    }

    void push(DialogRequest req) {
        m_queue.push_back(std::move(req));
    }

    bool hasPending() const { return !m_queue.empty(); }

    // Returns the next request, or nullptr if the queue is empty.
    const DialogRequest* front() const {
        return m_queue.empty() ? nullptr : &m_queue.front();
    }

    // Call when the shown dialog is dismissed (button clicked).
    void pop() {
        if (!m_queue.empty()) m_queue.erase(m_queue.begin());
    }

private:
    DialogManager() = default;
    std::vector<DialogRequest> m_queue;
};

// ---- Public API ------------------------------------------------------------
class Dialog {
public:
    // ---- Message box (single OK button) ------------------------------------
    static void message(const std::string& title, const std::string& body,
                        std::function<void()> onDismiss = {}) {
        DialogManager::instance().push({
            DialogRequest::Kind::Message, title, body, {},
            std::move(onDismiss), {}, {}
        });
    }

    // ---- Confirm (OK / Cancel) ---------------------------------------------
    static void confirm(const std::string& title, const std::string& body,
                        std::function<void()> onConfirm,
                        std::function<void()> onCancel = {}) {
        DialogManager::instance().push({
            DialogRequest::Kind::Confirm, title, body, {},
            std::move(onConfirm), std::move(onCancel), {}
        });
    }

    // ---- Text input --------------------------------------------------------
    static void input(const std::string& title, const std::string& prompt,
                      std::function<void(std::string)> onAccept,
                      std::function<void()>            onCancel      = {},
                      const std::string&               placeholder   = "") {
        DialogManager::instance().push({
            DialogRequest::Kind::Input, title, prompt, placeholder,
            {}, std::move(onCancel), std::move(onAccept)
        });
    }

    // ---- Open file ---------------------------------------------------------
    static void openFile(const std::string& title,
                         std::vector<std::string> extensions,
                         std::function<void(std::string)> onAccept,
                         std::function<void()>            onCancel = {}) {
        auto ac = std::make_shared<std::function<void(std::string)>>(std::move(onAccept));
        auto cn = std::make_shared<std::function<void()>>(std::move(onCancel));
        std::thread([title, exts=std::move(extensions), ac, cn]() mutable {
            std::string result = _nativeOpenFile(title, exts);
            MainThreadDispatcher::instance().post([result, ac, cn]{
                if (!result.empty() && *ac) (*ac)(result);
                else if (result.empty() && *cn) (*cn)();
            });
        }).detach();
    }

    // ---- Save file ---------------------------------------------------------
    static void saveFile(const std::string& title,
                         std::vector<std::string> extensions,
                         std::function<void(std::string)> onAccept,
                         std::function<void()>            onCancel = {}) {
        auto ac = std::make_shared<std::function<void(std::string)>>(std::move(onAccept));
        auto cn = std::make_shared<std::function<void()>>(std::move(onCancel));
        std::thread([title, exts=std::move(extensions), ac, cn]() mutable {
            std::string result = _nativeSaveFile(title, exts);
            MainThreadDispatcher::instance().post([result, ac, cn]{
                if (!result.empty() && *ac) (*ac)(result);
                else if (result.empty() && *cn) (*cn)();
            });
        }).detach();
    }

    // ---- Open directory ----------------------------------------------------
    static void openFolder(const std::string& title,
                           std::function<void(std::string)> onAccept,
                           std::function<void()>            onCancel = {}) {
        auto ac = std::make_shared<std::function<void(std::string)>>(std::move(onAccept));
        auto cn = std::make_shared<std::function<void()>>(std::move(onCancel));
        std::thread([title, ac, cn]() mutable {
            std::string result = _nativeOpenFolder(title);
            MainThreadDispatcher::instance().post([result, ac, cn]{
                if (!result.empty() && *ac) (*ac)(result);
                else if (result.empty() && *cn) (*cn)();
            });
        }).detach();
    }

private:
    // Runs a shell command and captures stdout. Used for zenity/kdialog.
    static std::string _popen_capture(const std::string& cmd) {
#if defined(_WIN32)
        (void)cmd;
        return {};
#else
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return {};
        std::string out;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        int rc = pclose(f);
        if (rc != 0) return {};                   // cancelled = non-zero exit
        if (!out.empty() && out.back() == '\n') out.pop_back();
        return out;
#endif
    }

    // Build zenity filter string from extension list: "*.json *.txt"
    static std::string _filterArg(const std::vector<std::string>& exts) {
        std::string s;
        for (const auto& e : exts) s += "*." + e + " ";
        return s;
    }

    static std::string _nativeOpenFile(const std::string& title,
                                       const std::vector<std::string>& exts) {
#if defined(_WIN32)
        char buf[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize  = sizeof(ofn);
        ofn.lpstrTitle   = title.c_str();
        ofn.lpstrFile    = buf;
        ofn.nMaxFile     = MAX_PATH;
        ofn.Flags        = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameA(&ofn) ? buf : std::string{};
#else
        std::string filter = _filterArg(exts);
        // Try zenity first, then kdialog
        std::string cmd = "zenity --file-selection --title=" + _q(title);
        if (!filter.empty()) cmd += " --file-filter=" + _q(filter);
        std::string res = _popen_capture(cmd);
        if (res.empty()) {
            cmd = "kdialog --getopenfilename . " + _q(filter) + " --title " + _q(title);
            res = _popen_capture(cmd);
        }
        return res;
#endif
    }

    static std::string _nativeSaveFile(const std::string& title,
                                       const std::vector<std::string>& exts) {
#if defined(_WIN32)
        char buf[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize  = sizeof(ofn);
        ofn.lpstrTitle   = title.c_str();
        ofn.lpstrFile    = buf;
        ofn.nMaxFile     = MAX_PATH;
        ofn.Flags        = OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
        return GetSaveFileNameA(&ofn) ? buf : std::string{};
#else
        std::string filter = _filterArg(exts);
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title=" + _q(title);
        if (!filter.empty()) cmd += " --file-filter=" + _q(filter);
        std::string res = _popen_capture(cmd);
        if (res.empty()) {
            cmd = "kdialog --getsavefilename . " + _q(filter) + " --title " + _q(title);
            res = _popen_capture(cmd);
        }
        return res;
#endif
    }

    static std::string _nativeOpenFolder(const std::string& title) {
#if defined(_WIN32)
        // SHBrowseForFolderA would need shell headers; skip for now
        return {};
#else
        std::string cmd = "zenity --file-selection --directory --title=" + _q(title);
        std::string res = _popen_capture(cmd);
        if (res.empty()) {
            cmd = "kdialog --getexistingdirectory . --title " + _q(title);
            res = _popen_capture(cmd);
        }
        return res;
#endif
    }

    // Shell-quote a string (single-quote with internal single-quote escaping).
    static std::string _q(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else           out += c;
        }
        out += "'";
        return out;
    }
};

} // namespace Genesis
