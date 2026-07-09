#pragma once

// ============================================================================
// JNativeDialogWindow — dialog as its own OS-level Popup window.
//
// Every behavior is driven by JDialogOptions on the request:
//   position        — CenterOnParent/CenterOnScreen/AtCursor/Fixed/corners
//   modal           — blocks parent-window input when true
//   draggable       — title-bar drag moves the OS window
//   constrainToScreen — clamp drag so the window never leaves the desktop
//   showTitleBar    — hide for minimal / toast-style dialogs
//   showCloseButton — hide the × when you don't want a close affordance
//   closeOnEscape   — Escape fires cancel / dismiss
//   closeOnClickOutside — dismiss when the dialog loses focus (non-modal use)
//   autoDismissMs   — auto-fire OK and close after N ms (message dialogs)
// ============================================================================

#include <j/core/Dialog.h>
#include <j/core/BaseWidgets.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JNativeDialogWindow {
public:
    static constexpr uint32_t kW      = 440;
    static constexpr float    kTitleH = 32.f;
    static constexpr float    kCloseW = 28.f;

#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JNativeDialogWindow(JDialogRequest req, JGpuHal& hal,
                       int screenX, int screenY,
                       NativeWinHandleType parentWindow = {})
        : m_req(std::move(req))
        , m_winH(_calcHeight(m_req.kind, m_req.options))
        , m_window(std::make_unique<PlatformWinType>(
              m_req.title.c_str(), kW, m_winH, screenX, screenY,
              JPlatformWindowStyle::Borderless,  // WM-managed: gets proper keyboard focus
              parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, m_winH))
        , m_createdAt(std::chrono::steady_clock::now())
    {}

    JNativeDialogWindow(const JNativeDialogWindow&)            = delete;
    JNativeDialogWindow& operator=(const JNativeDialogWindow&) = delete;
    JNativeDialogWindow(JNativeDialogWindow&&)                 = default;
    JNativeDialogWindow& operator=(JNativeDialogWindow&&)      = default;

    // Returns false when dismissed — caller should destroySurface() then erase.
    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;

        const auto& opts = m_req.options;

        // Auto-dismiss timer
        if (opts.autoDismissMs > 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - m_createdAt).count();
            if (ms >= opts.autoDismissMs) {
                if (m_req.onOk) m_req.onOk();
                _dismiss("auto");
                return false;
            }
        }

        m_window->pollNativeEvents();

        // Close-on-focus-loss (useful for non-modal tooltips / notifications)
        if (opts.closeOnClickOutside && m_window->consumeFocusLost()) {
            if (m_req.onCancel) m_req.onCancel();
            _dismiss("focus-lost");
            return false;
        }

        m_mx      = m_window->mouseX();
        m_my      = m_window->mouseY();
        m_pressed = m_window->consumePress();
        m_held    = m_window->isLeftButtonDown();
        m_keys    = m_window->consumeAllKeys();

        _handleKeys();
        if (m_done) return false;

        _handleDrag();

        buf.clear();
        _render(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool isDone()  const { return m_done; }
    bool isModal() const { return m_req.options.modal; }

    // Height calculation is public so main.cpp can compute the initial Y position.
    static uint32_t calcHeight(JDialogRequest::JKind k, const JDialogOptions& opts) {
        return _calcHeight(k, opts);
    }

private:
    static uint32_t _calcHeight(JDialogRequest::JKind k, const JDialogOptions& opts) {
        float contentH = (k == JDialogRequest::JKind::Input) ? 210.f : 155.f;
        return static_cast<uint32_t>((opts.showTitleBar ? kTitleH : 0.f) + contentH);
    }

    void _dismiss(const char* /*sig*/) {
        m_done = true;
    }

    void _handleKeys() {
        const auto& opts      = m_req.options;
        const bool needsInput = (m_req.kind == JDialogRequest::JKind::Input);
        const bool hasCancel  = (m_req.kind != JDialogRequest::JKind::Message);
        const JDialogKeyBindings& kb = JDialogManager::keyBindings();

        for (const auto& ke : m_keys) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;

            if (needsInput) {
                if      (ke.key == K::Backspace)  { if (!m_inputText.empty()) m_inputText.pop_back(); }
                else if (ke.key == kb.accept)     { if (m_req.onInput) m_req.onInput(m_inputText); _dismiss("input.accepted"); return; }
                else if (ke.key == kb.cancel && opts.closeOnEscape) { if (m_req.onCancel) m_req.onCancel(); _dismiss("cancelled"); return; }
                else if (ke.utf8[0] >= 0x20)      { m_inputText += ke.utf8; }
            } else {
                int n = hasCancel ? 2 : 1;
                if      (ke.key == kb.nextBtn)                      { m_focusedBtn = (m_focusedBtn + 1) % n; }
                else if (ke.key == kb.prevBtn)                      { m_focusedBtn = (m_focusedBtn + n - 1) % n; }
                else if (ke.key == kb.accept || ke.key == K::Space) {
                    if (m_focusedBtn == 0) { if (m_req.onOk) m_req.onOk(); _dismiss("ok"); }
                    else                   { if (m_req.onCancel) m_req.onCancel(); _dismiss("cancel"); }
                    return;
                }
                else if (ke.key == kb.cancel && opts.closeOnEscape) {
                    if (m_req.onCancel) m_req.onCancel();
                    _dismiss("cancel");
                    return;
                }
            }
        }
    }

    void _handleDrag() {
        const auto& opts = m_req.options;
        if (!opts.draggable || !opts.showTitleBar) return;

        float titleDragW = (float)kW - (opts.showCloseButton ? kCloseW : 0.f);
        bool inTitle = (m_mx >= 0.f && m_mx < titleDragW && m_my >= 0.f && m_my < kTitleH);

        if (m_held && inTitle && !m_dragging) {
            m_dragging    = true;
            m_dragAnchorX = m_mx;
            m_dragAnchorY = m_my;
        }
        if (m_dragging) {
            auto [gx, gy] = m_window->globalCursorPos();
            int nx = gx - static_cast<int>(m_dragAnchorX);
            int ny = gy - static_cast<int>(m_dragAnchorY);
            if (opts.constrainToScreen) {
                auto [sw, sh] = m_window->virtualDesktopSize();
                nx = std::max(0, std::min(nx, sw - (int)kW));
                ny = std::max(0, std::min(ny, sh - (int)m_winH));
            }
            m_window->setPosition(nx, ny);
        }
        if (!m_held) m_dragging = false;
    }

    void _render(JPrimitiveBuffer& buf) {
        const auto& opts = m_req.options;
        const float W = static_cast<float>(kW);
        const float H = static_cast<float>(m_winH);
        const bool  needsInput = (m_req.kind == JDialogRequest::JKind::Input);
        const bool  hasCancel  = (m_req.kind != JDialogRequest::JKind::Message);
        const float lh = JTextHelper::lineHeight();

        // JWindow background
        uint8_t bg[4] = {26, 26, 32, 255};
        buf.pushRectangle(0.f, 0.f, W, H, bg, 8.f, 1.f, Colors::Border);

        float contentY = 0.f;

        // Title bar
        if (opts.showTitleBar) {
            uint8_t tbg[4] = {38, 38, 48, 255};
            buf.pushRectangle(0.f, 0.f, W, kTitleH, tbg, 8.f);
            buf.pushRectangle(0.f, 8.f, W, kTitleH - 8.f, tbg, 0.f);

            float titleMaxW = opts.showCloseButton ? W - kCloseW - 20.f : W - 20.f;
            uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
            JTextHelper::pushText(buf, 14.f, (kTitleH - lh) * 0.5f, m_req.title, tc, titleMaxW);

            // Close × button
            if (opts.showCloseButton) {
                bool hovClose = (m_mx >= W - kCloseW && m_mx < W && m_my >= 0.f && m_my < kTitleH);
                const float sz = 18.f, cbx = W - kCloseW + (kCloseW - sz) * 0.5f, cby = (kTitleH - sz) * 0.5f;
                jDrawCloseButton(buf, cbx, cby, sz, hovClose);
                if (m_pressed && hovClose) { if (m_req.onCancel) m_req.onCancel(); _dismiss("close"); return; }
            }

            contentY = kTitleH;
        }

        // Body text
        float ty = contentY + 16.f;
        uint8_t sc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sc);
        JTextHelper::pushText(buf, 16.f, ty, m_req.body, sc, W - 32.f);
        ty += lh + 10.f;

        // Input field
        if (needsInput) {
            float fW = W - 32.f, fH = 32.f;
            uint8_t fBg[4] = {18,18,28,255};
            uint8_t fBorder[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 255};
            buf.pushRectangle(16.f, ty, fW, fH, fBg, 4.f, 2.f, fBorder);
            float textY = ty + (fH - lh) * 0.5f;
            if (m_inputText.empty()) {
                uint8_t ph[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, ph);
                JTextHelper::pushText(buf, 24.f, textY, m_req.placeholder, ph, fW - 16.f);
            } else {
                uint8_t ftc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, ftc);
                JTextHelper::pushText(buf, 24.f, textY, m_inputText, ftc, fW - 16.f);
            }
            float cursorX = 24.f + JTextHelper::measureWidth(m_inputText) + 1.f;
            uint8_t cc[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 220};
            buf.pushRectangle(cursorX, textY + 1.f, 2.f, lh - 2.f, cc, 0.f);
        }

        // Buttons — order driven by opts.okOnRight
        const auto& okLbl     = opts.okLabel;
        const auto& cancelLbl = opts.cancelLabel;
        float btnW = 88.f, btnH = 30.f;
        float btnY = H - btnH - 14.f;
        float okX, cancelX;
        if (opts.okOnRight) {
            okX     = W - btnW - 12.f;
            cancelX = hasCancel ? W - btnW * 2.f - 20.f : 0.f;
        } else {
            cancelX = hasCancel ? W - btnW - 12.f : 0.f;
            okX     = hasCancel ? W - btnW * 2.f - 20.f : (W - btnW) * 0.5f;
        }
        uint8_t btnTc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, btnTc);

        bool hovOk = (m_mx >= okX && m_mx < okX + btnW && m_my >= btnY && m_my < btnY + btnH);
        bool kbOk  = !needsInput && (m_focusedBtn == 0);
        uint8_t okBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2],
                            static_cast<uint8_t>(hovOk ? 255 : 220)};
        buf.pushRectangle(okX, btnY, btnW, btnH, okBg, 4.f);
        if (kbOk) {
            uint8_t nf[4]={0,0,0,0}, rg[4]={255,255,255,200};
            buf.pushRectangle(okX-2.f, btnY-2.f, btnW+4.f, btnH+4.f, nf, 5.f, 2.f, rg);
        }
        JTextHelper::pushText(buf, okX + (btnW - JTextHelper::measureWidth(okLbl.c_str())) * 0.5f,
                             btnY + (btnH - lh) * 0.5f, okLbl, btnTc);
        if (m_pressed && hovOk) {
            if (needsInput) { if (m_req.onInput) m_req.onInput(m_inputText); }
            else             { if (m_req.onOk)   m_req.onOk(); }
            _dismiss("ok"); return;
        }

        if (hasCancel) {
            bool hovCancel = (m_mx >= cancelX && m_mx < cancelX + btnW && m_my >= btnY && m_my < btnY + btnH);
            bool kbCancel  = !needsInput && (m_focusedBtn == 1);
            uint8_t cBg[4] = {55,55,65, static_cast<uint8_t>(hovCancel ? 255 : 220)};
            uint8_t cBorder[4] = {100,100,110,255};
            buf.pushRectangle(cancelX, btnY, btnW, btnH, cBg, 4.f, 1.f, cBorder);
            if (kbCancel) {
                uint8_t nf[4]={0,0,0,0}, rg[4]={255,255,255,200};
                buf.pushRectangle(cancelX-2.f, btnY-2.f, btnW+4.f, btnH+4.f, nf, 5.f, 2.f, rg);
            }
            JTextHelper::pushText(buf, cancelX + (btnW - JTextHelper::measureWidth(cancelLbl.c_str())) * 0.5f,
                                 btnY + (btnH - lh) * 0.5f, cancelLbl, btnTc);
            if (m_pressed && hovCancel) {
                if (m_req.onCancel) m_req.onCancel();
                _dismiss("cancel"); return;
            }
        }
    }

    JDialogRequest                    m_req;
    uint32_t                         m_winH;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId                     m_surface{0};
    std::chrono::steady_clock::time_point m_createdAt;
    bool                             m_done{false};
    bool                             m_dragging{false};
    float                            m_dragAnchorX{0}, m_dragAnchorY{0};
    std::string                      m_inputText;
    int                              m_focusedBtn{0};
    float                            m_mx{0}, m_my{0};
    bool                             m_pressed{false}, m_held{false};
    std::vector<JKeyEvent>            m_keys;
};

} // inline namespace jf
