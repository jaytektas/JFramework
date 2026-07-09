#pragma once

// ============================================================================
// JDialog — non-blocking modal dialog system (Step 2)
//
// Superior to Qt: no exec() spin-loop, no QDialog subclass required.
// All methods return immediately; results arrive via lambda callback.
//
// Genesis-native dialogs (message, confirm, input):
//   - Rendered as JPopupWindow overlays inside the Genesis compositor
//   - Zero OS dependencies, consistent look across platforms
//
// File/folder/color pickers use OS-native dialogs via a background thread
//   - Linux: zenity (Wayland/X11) or kdialog as fallback
//   - Windows: GetOpenFileNameA / GetSaveFileNameA (commdlg.h)
//   - macOS: NSOpenPanel / NSSavePanel via AppleScript (no Obj-C required)
//
// All callbacks are posted back to the main thread via JMainThreadDispatcher.
// ============================================================================

#include "Signal.h"
#include "MainThreadDispatcher.h"
#include "JTextHelper.h"
#include "JStyle.h"
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

inline namespace jf {

// ---- JDialogKeyBindings -----------------------------------------------------
// Configurable key bindings for dialog button navigation.
// Defaults follow standard platform conventions (Enter=OK, Escape=Cancel).
struct JDialogKeyBindings {
    JKeyEvent::JKey accept  = JKeyEvent::JKey::Return;
    JKeyEvent::JKey cancel  = JKeyEvent::JKey::Escape;
    JKeyEvent::JKey nextBtn = JKeyEvent::JKey::Tab;
    JKeyEvent::JKey prevBtn = JKeyEvent::JKey::BackTab;
};

// ---- JDialogOptions ---------------------------------------------------------
// Per-dialog options — passed to JDialog::message/confirm/input.
// Follows the JFloatingDockOptions pattern: named fields with safe defaults.
struct JDialogOptions {

    // --- JPosition -----------------------------------------------------------
    enum class JPosition : uint8_t {
        CenterOnParent,   // centred over the spawning window (default)
        CenterOnScreen,   // centred on the primary monitor
        AtCursor,         // top-left at the current cursor position
        Fixed,            // explicit (x, y) below
        TopLeft,          // relative to parent window corners / edges
        TopRight,
        TopCenter,
        BottomLeft,
        BottomRight,
        BottomCenter,
    };
    JPosition position = JPosition::CenterOnParent;
    int x = 0, y = 0;          // used when position == Fixed

    // --- Modality -----------------------------------------------------------
    bool modal              = true;   // blocks input to the parent window
    bool closeOnClickOutside = false; // dismiss when focus is lost (non-modal)

    // --- Drag / resize ------------------------------------------------------
    bool draggable          = true;   // title-bar drag moves the OS window
    bool constrainToScreen  = false;  // clamp drag so the window stays on-screen

    // --- Chrome -------------------------------------------------------------
    bool showTitleBar       = true;
    bool showCloseButton    = true;

    // --- Behaviour ----------------------------------------------------------
    bool  closeOnEscape     = true;   // Escape key fires cancel/dismiss
    int   autoDismissMs     = 0;      // >0: auto-dismiss after N milliseconds (message only)

    // --- JButton layout ------------------------------------------------------
    bool  okOnRight         = true;   // false = OK left, Cancel right (legacy/Linux convention)
    std::string okLabel     = "OK";
    std::string cancelLabel = "Cancel";
};

// ---- JDialogRequest ---------------------------------------------------------
// Describes a pending genesis-native overlay dialog.
// JDialogManager services one at a time; queue subsequent requests.
struct JDialogRequest {
    enum class JKind { Message, Confirm, Input };
    JKind          kind{JKind::Message};
    std::string   title;
    std::string   body;
    std::string   placeholder;
    JDialogOptions options;
    std::function<void()>               onOk;
    std::function<void()>               onCancel;
    std::function<void(std::string)>    onInput;
};

// ---- JDialogManager ---------------------------------------------------------
// Populated by JDialog::* static methods; drained by the platform window each frame.
class JDialogManager {
public:
    static JDialogManager& instance() {
        static JDialogManager dm;
        return dm;
    }

    void push(JDialogRequest req) {
        m_queue.push_back(std::move(req));
    }

    bool hasPending() const { return !m_queue.empty(); }

    // Returns the next request, or nullptr if the queue is empty.
    const JDialogRequest* front() const {
        return m_queue.empty() ? nullptr : &m_queue.front();
    }

    // ---- Genesis-native overlay renderer -----------------------------------
    // Call once per frame after rendering the main widget tree but before
    // hal.drawPrimitives(). Pass the already-consumed `pressed` from the main
    // event loop — do NOT call consumePress() again here.
    // Returns true if a dialog is active (all input consumed).
    static bool renderAndHandle(JPrimitiveBuffer& buf,
                                float screenW, float screenH,
                                float mx, float my,
                                bool mousePressed,   // one-shot press event — for button clicks
                                bool mouseHeld,      // continuous held state — for drag
                                const std::vector<JKeyEvent>& keys = {})
    {
        auto& dm  = instance();
        const JDialogRequest* req = dm.front();
        if (!req) return false;

        static constexpr float kTitleH = 32.f;
        static constexpr float kCloseW = 28.f;
        static constexpr float kRadius = 8.f;
        const bool hasCancel = (req->kind != JDialogRequest::JKind::Message);
        const bool needsInput = (req->kind == JDialogRequest::JKind::Input);
        const JDialogKeyBindings& kb = dm.m_keyBindings;

        // --- JKey input (processed before drawing so state is current this frame) ---
        for (const auto& ke : keys) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;

            if (needsInput) {
                // Text field gets printable keys; Enter/Escape are accept/cancel
                if (ke.key == K::Backspace) {
                    if (!dm.m_inputText.empty()) dm.m_inputText.pop_back();
                } else if (ke.key == kb.accept) {
                    std::string txt = dm.m_inputText;
                    if (req->onInput) req->onInput(txt);
                    dm.pop(); return true;
                } else if (ke.key == kb.cancel) {
                    if (req->onCancel) req->onCancel();
                    dm.pop(); return true;
                } else if (ke.utf8[0] >= 0x20) {
                    dm.m_inputText += ke.utf8;
                }
            } else {
                // Message / Confirm: navigate and activate buttons with keyboard
                int btnCount = hasCancel ? 2 : 1;
                if (ke.key == kb.nextBtn) {
                    dm.m_focusedBtn = (dm.m_focusedBtn + 1) % btnCount;
                } else if (ke.key == kb.prevBtn) {
                    dm.m_focusedBtn = (dm.m_focusedBtn + btnCount - 1) % btnCount;
                } else if (ke.key == kb.accept || ke.key == K::Space) {
                    if (dm.m_focusedBtn == 0) {
                        if (req->onOk) req->onOk();
                    } else {
                        if (req->onCancel) req->onCancel();
                    }
                    dm.pop(); return true;
                } else if (ke.key == kb.cancel) {
                    if (req->onCancel) req->onCancel();
                    dm.pop(); return true;
                }
            }
        }

        // --- Backdrop ---
        if (req->options.modal) {
            buf.pushRectangle(0.f, 0.f, screenW, screenH, Colors::OverlayScrim, 0.f);
        }

        // --- Box geometry (position may be offset by drag) ---
        float boxW = std::min(440.f, screenW * 0.8f);
        float boxH = kTitleH + (needsInput ? 200.f : 140.f);

        // Initialise drag offset on first frame for this dialog
        if (!dm.m_dragInit) {
            dm.m_offsetX = (screenW - boxW) * 0.5f;
            dm.m_offsetY = (screenH - boxH) * 0.5f;
            dm.m_dragInit = true;
        }
        float boxX = dm.m_offsetX;
        float boxY = dm.m_offsetY;

        // --- Drag title bar ---
        // Use mouseHeld (continuous) for drag — mousePressed is one-shot and
        // would drop the drag after the first frame.
        bool inTitle = (mx >= boxX && mx < boxX + boxW - kCloseW &&
                        my >= boxY && my < boxY + kTitleH);
        if (mouseHeld && inTitle && !dm.m_dragging) {
            dm.m_dragging    = true;
            dm.m_dragAnchorX = mx - boxX;
            dm.m_dragAnchorY = my - boxY;
        }
        if (dm.m_dragging) {
            float nx = mx - dm.m_dragAnchorX;
            float ny = my - dm.m_dragAnchorY;
            if (req->options.constrainToScreen) {
                nx = std::clamp(nx, 0.f, screenW - boxW);
                ny = std::clamp(ny, 0.f, screenH - boxH);
            }
            dm.m_offsetX = nx;
            dm.m_offsetY = ny;
            boxX = nx;
            boxY = ny;
        }
        if (!mouseHeld) dm.m_dragging = false;

        // --- Shadow (slight dark halo) ---
        buf.pushRectangle(boxX + 4.f, boxY + 6.f, boxW, boxH, Colors::DialogShadow, kRadius + 2.f);

        // --- Box body ---
        buf.pushRectangle(boxX, boxY, boxW, boxH, Colors::DialogBg, kRadius, 1.f, Colors::Border);

        // --- Title bar strip ---
        buf.pushRectangle(boxX, boxY, boxW, kTitleH, Colors::DialogTitleBg, kRadius);
        // Cover bottom corners of title so it blends into body
        buf.pushRectangle(boxX, boxY + kRadius, boxW, kTitleH - kRadius, Colors::DialogTitleBg, 0.f);

        float lh = JTextHelper::lineHeight();
        uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
        JTextHelper::pushText(buf, boxX + 14.f, boxY + (kTitleH - lh) * 0.5f, req->title, tc,
                             boxW - kCloseW - 20.f);

        // --- Close (×) button ---
        float cBtnX = boxX + boxW - kCloseW;
        float cBtnY = boxY;
        bool  hovClose = (mx >= cBtnX && mx < cBtnX + kCloseW &&
                          my >= cBtnY && my < cBtnY + kTitleH);
        if (hovClose) {
            buf.pushRectangle(cBtnX, cBtnY, kCloseW, kTitleH, Colors::DialogCloseHover, 0.f);
        }
        uint8_t xc[4] = {Colors::TitleBarText[0], Colors::TitleBarText[1], Colors::TitleBarText[2], 220};
        float cx = cBtnX + kCloseW * 0.5f - 4.f;
        float cy = cBtnY + kTitleH  * 0.5f - 1.f;
        buf.pushRectangle(cx,        cy,        9.f, 2.f, xc, 1.f);  // —
        buf.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, xc, 1.f); // |
        if (mousePressed && hovClose) {
            if (req->onCancel) req->onCancel();
            dm.pop();
            return true;
        }

        // --- Body / prompt ---
        float ty = boxY + kTitleH + 16.f;
        uint8_t sc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sc);
        JTextHelper::pushText(buf, boxX + 16.f, ty, req->body, sc, boxW - 32.f);
        ty += lh + 10.f;

        // --- Input field (always active — dialog owns keyboard) ---
        if (needsInput) {
            float fieldX = boxX + 16.f;
            float fieldW = boxW - 32.f;
            float fieldH = 32.f;

            uint8_t fBorder[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 255};
            buf.pushRectangle(fieldX, ty, fieldW, fieldH, Colors::InputFieldBg, 4.f, 2.f, fBorder);

            float textY = ty + (fieldH - lh) * 0.5f;
            if (dm.m_inputText.empty()) {
                uint8_t ph[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, ph);
                JTextHelper::pushText(buf, fieldX + 8.f, textY, req->placeholder, ph, fieldW - 24.f);
            } else {
                uint8_t ftc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, ftc);
                JTextHelper::pushText(buf, fieldX + 8.f, textY, dm.m_inputText, ftc, fieldW - 24.f);
            }
            float cursorX = fieldX + 8.f + JTextHelper::measureWidth(dm.m_inputText) + 1.f;
            uint8_t cursorCol[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 220};
            buf.pushRectangle(cursorX, textY + 1.f, 2.f, lh - 2.f, cursorCol, 0.f);

            ty += fieldH + 8.f;
        }

        // --- Buttons ---
        float btnW    = 88.f;
        float btnH    = 30.f;
        float btnY    = boxY + boxH - btnH - 14.f;
        float okX     = hasCancel ? boxX + boxW - btnW * 2.f - 20.f
                                  : boxX + (boxW - btnW) * 0.5f;
        float cancelX = boxX + boxW - btnW - 12.f;

        uint8_t btnTc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, btnTc);

        // Clamp keyboard focus to available buttons
        if (!hasCancel) dm.m_focusedBtn = 0;

        // OK button — keyboard-focused when m_focusedBtn==0
        bool hovOk = (mx >= okX && mx < okX + btnW && my >= btnY && my < btnY + btnH);
        bool kbOk  = !needsInput && (dm.m_focusedBtn == 0);
        uint8_t okBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2],
                            static_cast<uint8_t>(hovOk ? 255 : 220)};
        buf.pushRectangle(okX, btnY, btnW, btnH, okBg, 4.f);
        if (kbOk) { // outline-only focus ring — does not cover button fill
            buf.pushRectangle(okX - 2.f, btnY - 2.f, btnW + 4.f, btnH + 4.f, Colors::Transparent, 5.f, 2.f, Colors::CloseBtnMark);
        }
        JTextHelper::pushText(buf, okX + (btnW - JTextHelper::measureWidth("OK")) * 0.5f,
                             btnY + (btnH - lh) * 0.5f, "OK", btnTc);

        if (mousePressed && hovOk) {
            if (needsInput) { if (req->onInput) req->onInput(dm.m_inputText); }
            else             { if (req->onOk)   req->onOk(); }
            dm.pop(); return true;
        }

        // Cancel button — keyboard-focused when m_focusedBtn==1
        if (hasCancel) {
            bool hovCancel = (mx >= cancelX && mx < cancelX + btnW &&
                              my >= btnY    && my < btnY + btnH);
            bool kbCancel  = !needsInput && (dm.m_focusedBtn == 1);
            uint8_t cBg[4] = {Colors::CancelBtnBg[0], Colors::CancelBtnBg[1], Colors::CancelBtnBg[2], static_cast<uint8_t>(hovCancel ? 255 : 220)};
            buf.pushRectangle(cancelX, btnY, btnW, btnH, cBg, 4.f, 1.f, Colors::CancelBtnBorder);
            if (kbCancel) {
                buf.pushRectangle(cancelX - 2.f, btnY - 2.f, btnW + 4.f, btnH + 4.f, Colors::Transparent, 5.f, 2.f, Colors::CloseBtnMark);
            }
            JTextHelper::pushText(buf, cancelX + (btnW - JTextHelper::measureWidth("Cancel")) * 0.5f,
                                 btnY + (btnH - lh) * 0.5f, "Cancel", btnTc);

            if (mousePressed && hovCancel) {
                if (req->onCancel) req->onCancel();
                dm.pop(); return true;
            }
        }

        return true;
    }

    // Programmatic action support — dispatch a named action to the front dialog.
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

    // Also reset drag state when queue becomes empty
    void pop() {
        if (!m_queue.empty()) m_queue.erase(m_queue.begin());
        m_inputText.clear();
        if (m_queue.empty()) {
            m_dragInit   = false;
            m_dragging   = false;
            m_focusedBtn = 0;
        }
    }

    // Configurable key bindings (set before showing dialogs)
    static JDialogKeyBindings& keyBindings() { return instance().m_keyBindings; }

    // True if the front dialog is modal (should block underlying input).
    static bool isModal() {
        const JDialogRequest* r = instance().front();
        return r && r->options.modal;
    }

private:
    JDialogManager() = default;
    std::vector<JDialogRequest> m_queue;
    std::string        m_inputText;
    int                m_focusedBtn = 0;
    JDialogKeyBindings  m_keyBindings;

    // Drag state for the title-bar drag gesture
    bool  m_dragInit    = false;
    bool  m_dragging    = false;
    float m_offsetX     = 0.f;
    float m_offsetY     = 0.f;
    float m_dragAnchorX = 0.f;
    float m_dragAnchorY = 0.f;
};

// ---- Public API ------------------------------------------------------------
class JDialog {
public:
    // ---- Message box (single OK button) ------------------------------------
    static void message(const std::string& title, const std::string& body,
                        std::function<void()> onDismiss = {},
                        JDialogOptions opts = {}) {
        JDialogRequest req;
        req.kind = JDialogRequest::JKind::Message;
        req.title = title; req.body = body;
        req.options = opts;
        req.onOk = std::move(onDismiss);
        JDialogManager::instance().push(std::move(req));
    }

    // ---- Confirm (OK / Cancel) ---------------------------------------------
    static void confirm(const std::string& title, const std::string& body,
                        std::function<void()> onConfirm,
                        std::function<void()> onCancel = {},
                        JDialogOptions opts = {}) {
        JDialogRequest req;
        req.kind = JDialogRequest::JKind::Confirm;
        req.title = title; req.body = body;
        req.options = opts;
        req.onOk = std::move(onConfirm);
        req.onCancel = std::move(onCancel);
        JDialogManager::instance().push(std::move(req));
    }

    // ---- Text input --------------------------------------------------------
    static void input(const std::string& title, const std::string& prompt,
                      std::function<void(std::string)> onAccept,
                      std::function<void()>            onCancel    = {},
                      const std::string&               placeholder = "",
                      JDialogOptions                    opts        = {}) {
        JDialogRequest req;
        req.kind = JDialogRequest::JKind::Input;
        req.title = title; req.body = prompt; req.placeholder = placeholder;
        req.options = opts;
        req.onCancel = std::move(onCancel);
        req.onInput  = std::move(onAccept);
        JDialogManager::instance().push(std::move(req));
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
            JMainThreadDispatcher::instance().post([result, ac, cn]{
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
            JMainThreadDispatcher::instance().post([result, ac, cn]{
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
            JMainThreadDispatcher::instance().post([result, ac, cn]{
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

} // inline namespace jf
