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
#include "AiBusHook.h"
#include "BaseWidgets.h"
#include "KeyEvent.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <cstdio>
#include <memory>
#include <algorithm>

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
        m_inputText.clear();
    }

    // ---- Genesis-native overlay renderer -----------------------------------
    // Call once per frame after rendering the main widget tree but before
    // hal.drawPrimitives(). Returns true if a dialog is active (input consumed).
    //
    // Parameters:
    //   buf       — PrimitiveBuffer to push overlay draw commands into
    //   screenW/H — window size in pixels
    //   mx, my    — current mouse position
    //   mouseDown — true the frame the primary button is pressed
    //   keys      — key events this frame (used for input-dialog text entry)
    static bool renderAndHandle(PrimitiveBuffer& buf,
                                float screenW, float screenH,
                                float mx, float my,
                                bool mouseDown,
                                const std::vector<KeyEvent>& keys = {})
    {
        auto& dm  = instance();
        const DialogRequest* req = dm.front();
        if (!req) return false;

        // --- Backdrop ---
        uint8_t backdrop[4] = {0, 0, 0, 160};
        buf.pushRectangle(0.f, 0.f, screenW, screenH, backdrop, 0.f);

        // --- Box ---
        float boxW = std::min(440.f, screenW * 0.8f);
        bool  needsInput = (req->kind == DialogRequest::Kind::Input);
        float boxH = needsInput ? 210.f : 160.f;
        float boxX = (screenW - boxW) * 0.5f;
        float boxY = (screenH - boxH) * 0.5f;

        uint8_t boxBg[4]  = {30, 30, 36, 255};
        buf.pushRectangle(boxX, boxY, boxW, boxH, boxBg, 8.f, 1.f, Colors::Border);

        float lh = TextHelper::lineHeight();

        // --- Title ---
        uint8_t tc[4];
        std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
        float ty = boxY + 20.f;
        TextHelper::pushText(buf, boxX + 20.f, ty, req->title, tc, boxW - 40.f);

        // --- Body / prompt ---
        uint8_t sc[4];
        std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sc);
        ty += lh + 8.f;
        TextHelper::pushText(buf, boxX + 20.f, ty, req->body, sc, boxW - 40.f);

        // --- Input field ---
        if (needsInput) {
            ty += lh + 10.f;
            float fieldX = boxX + 20.f;
            float fieldW = boxW - 40.f;
            float fieldH = 30.f;
            uint8_t fBg[4] = {20, 20, 24, 255};
            buf.pushRectangle(fieldX, ty, fieldW, fieldH, fBg, 4.f, 1.f, Colors::Accent);

            // Handle key events for text entry
            for (const auto& ke : keys) {
                if (!ke.pressed) continue;
                using K = KeyEvent::Key;
                if (ke.key == K::Backspace) {
                    if (!dm.m_inputText.empty()) dm.m_inputText.pop_back();
                } else if (ke.key == K::Return) {
                    std::string txt = dm.m_inputText;
                    if (req->onInput) req->onInput(txt);
                    if (AiBusHook::emit) AiBusHook::emit(0, "dialog.dismissed", "input.accepted");
                    dm.pop();
                    return true;
                } else if (ke.key == K::Escape) {
                    if (req->onCancel) req->onCancel();
                    if (AiBusHook::emit) AiBusHook::emit(0, "dialog.dismissed", "input.cancelled");
                    dm.pop();
                    return true;
                } else if (ke.utf8[0] >= 0x20) {
                    dm.m_inputText += ke.utf8;
                }
            }

            const std::string& displayText = dm.m_inputText.empty() ? req->placeholder : dm.m_inputText;
            const uint8_t* dtc = dm.m_inputText.empty() ? Colors::TextSecondary : Colors::TextPrimary;
            uint8_t dtcArr[4]; std::copy(dtc, dtc + 4, dtcArr);
            if (!displayText.empty())
                TextHelper::pushText(buf, fieldX + 8.f, ty + (fieldH - lh) * 0.5f, displayText, dtcArr, fieldW - 16.f);

            ty += fieldH;
        }

        // --- Buttons ---
        bool hasCancel = (req->kind != DialogRequest::Kind::Message);
        float btnW    = 88.f;
        float btnH    = 30.f;
        float btnY    = boxY + boxH - btnH - 16.f;
        float okX     = hasCancel ? boxX + boxW - btnW * 2.f - 24.f
                                  : boxX + (boxW - btnW) * 0.5f;
        float cancelX = boxX + boxW - btnW - 16.f;

        // OK button
        bool hovOk = (mx >= okX && mx < okX + btnW && my >= btnY && my < btnY + btnH);
        uint8_t okBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2],
                            static_cast<uint8_t>(hovOk ? 255 : 200)};
        buf.pushRectangle(okX, btnY, btnW, btnH, okBg, 4.f);
        float okTx = okX + (btnW - TextHelper::measureWidth("OK")) * 0.5f;
        uint8_t btnTc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, btnTc);
        TextHelper::pushText(buf, okTx, btnY + (btnH - lh) * 0.5f, "OK", btnTc);

        if (mouseDown && hovOk) {
            if (needsInput) {
                std::string txt = dm.m_inputText;
                if (req->onInput) req->onInput(txt);
            } else {
                if (req->onOk) req->onOk();
            }
            if (AiBusHook::emit) AiBusHook::emit(0, "dialog.dismissed", "ok");
            dm.pop();
            return true;
        }

        // Cancel button
        if (hasCancel) {
            bool hovCancel = (mx >= cancelX && mx < cancelX + btnW && my >= btnY && my < btnY + btnH);
            uint8_t cBg[4] = {50, 50, 58, static_cast<uint8_t>(hovCancel ? 255 : 200)};
            buf.pushRectangle(cancelX, btnY, btnW, btnH, cBg, 4.f, 1.f, Colors::Border);
            float cTx = cancelX + (btnW - TextHelper::measureWidth("Cancel")) * 0.5f;
            TextHelper::pushText(buf, cTx, btnY + (btnH - lh) * 0.5f, "Cancel", btnTc);

            if (mouseDown && hovCancel) {
                if (req->onCancel) req->onCancel();
                if (AiBusHook::emit) AiBusHook::emit(0, "dialog.dismissed", "cancel");
                dm.pop();
                return true;
            }
        }

        return true; // dialog active, consumed this frame
    }

    // AI agent action support
    static bool executeAction(const std::string& action) {
        auto& dm = instance();
        if (!dm.front()) return false;
        if (action == "ok")     { dm.front()->onOk ? dm.front()->onOk() : void(); dm.pop(); return true; }
        if (action == "cancel") { dm.front()->onCancel ? dm.front()->onCancel() : void(); dm.pop(); return true; }
        if (action.rfind("input:", 0) == 0) {
            dm.m_inputText = action.substr(6);
            return true;
        }
        return false;
    }

private:
    DialogManager() = default;
    std::vector<DialogRequest> m_queue;
    std::string m_inputText;
};

// ---- Public API ------------------------------------------------------------
class Dialog {
public:
    // ---- Message box (single OK button) ------------------------------------
    static void message(const std::string& title, const std::string& body,
                        std::function<void()> onDismiss = {}) {
        if (AiBusHook::emit) AiBusHook::emit(0, "dialog.message", title.c_str());
        DialogManager::instance().push({
            DialogRequest::Kind::Message, title, body, {},
            std::move(onDismiss), {}, {}
        });
    }

    // ---- Confirm (OK / Cancel) ---------------------------------------------
    static void confirm(const std::string& title, const std::string& body,
                        std::function<void()> onConfirm,
                        std::function<void()> onCancel = {}) {
        if (AiBusHook::emit) AiBusHook::emit(0, "dialog.confirm", title.c_str());
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
        if (AiBusHook::emit) AiBusHook::emit(0, "dialog.input", title.c_str());
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
