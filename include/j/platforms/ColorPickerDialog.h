#pragma once

// ============================================================================
// JColorPickerDialog — the colour chooser as a modal dialog window, matching the studio's picker.
//
// One window, one "Cancel | Colour | Select" header, two pages:
//   • Palette  — a hue×shade grid of preset swatches (checkmark on the current one) plus a "Custom"
//                row of previously-defined colours and a "+" that opens the editor.
//   • Editor   — the "+" custom page: the JColorPicker (hue bar, SV square, eyedropper, hex, alpha).
//
// It is a real WM-managed window (keyboard focus, no pointer grab, draggable by the header) — it does
// not dismiss on click-outside. Select applies the working colour (onAccept); Cancel/close/Escape
// discard. On the editor page Cancel returns to the palette; Select applies + remembers the custom.
// Modelled on JNativeDialogWindow's window/surface/drag machinery.
// ============================================================================

#include <j/core/ColorPicker.h>
#include <j/config/Settings.h>          // JSettings — persist the custom-colour row
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/VectorGraphics.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JColorPickerDialog {
public:
    static constexpr uint32_t kW      = 452;
    static constexpr float    kHeader = 46.f;
    static constexpr float    kCloseW = 28.f;
    static constexpr float    kBtnW   = 84.f;
    static constexpr float    kBtnH   = 30.f;
    // Palette-page grid.
    static constexpr int   kCols = 8, kRows = 5;
    static constexpr float kCellW = 48.f, kCellH = 32.f, kGap = 6.f, kPad = 13.f;
    static constexpr float kPaletteH = kHeader + 16.f + (kRows * kCellH + (kRows - 1) * kGap) + 16.f + 22.f + kCellH + 14.f;
    static constexpr float kEditorH  = kHeader + 10.f + JColorPicker::kH + 14.f;

#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JColorPickerDialog(const std::string& startHex, JGpuHal& hal,
                       int screenX, int screenY, NativeWinHandleType parentWindow,
                       std::function<void(std::string)> onAccept,
                       std::function<void()>            onCancel = {})
        : m_onAccept(std::move(onAccept)), m_onCancel(std::move(onCancel)), m_working(startHex)
        , m_window(std::make_unique<PlatformWinType>("Colour", kW, static_cast<uint32_t>(kPaletteH), screenX, screenY,
                                                     JPlatformWindowStyle::Borderless, parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, static_cast<uint32_t>(kPaletteH)))
        , m_curH(static_cast<uint32_t>(kPaletteH))
    {
        m_picker = std::make_unique<JColorPicker>(m_graph);
        auto& l = m_graph.getLayout(m_picker->getNodeId());
        l.boundingBox.x = (kW - JColorPicker::kW) * 0.5f;
        l.boundingBox.y = kHeader + 10.f;
        m_picker->onColorChanged.connect([this](const std::string& hex) { m_working = hex; });
    }

    JColorPickerDialog(const JColorPickerDialog&)            = delete;
    JColorPickerDialog& operator=(const JColorPickerDialog&) = delete;

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }
    bool isDone() const { return m_done; }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { _cancel(); return false; }

        // Keep the window height matched to the active page.
        const uint32_t wantH = static_cast<uint32_t>(m_page == Page::Editor ? kEditorH : kPaletteH);
        if (wantH != m_curH) { m_curH = wantH; m_window->setSize(kW, wantH); hal.resizeSurface(m_surface, kW, wantH); }

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed  = m_window->consumePress();
        const bool released = m_window->consumeRelease();
        const bool held     = m_window->isLeftButtonDown();

        _handleDrag(mx, my, held);

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) { if (m_page == Page::Editor) { m_page = Page::Palette; continue; } _cancel(); return false; }
            if (m_page == Page::Editor && m_picker->handleKeyEvent(ke)) continue;
            if (ke.key == K::Return) { _select(); if (m_done) return false; continue; }
        }

        // Header buttons (both pages).
        const float W = static_cast<float>(kW);
        const float btnY = (kHeader - kBtnH) * 0.5f;
        const float cancelX = 12.f, selectX = W - kBtnW - 12.f;
        if (pressed && my >= btnY && my < btnY + kBtnH) {
            if (mx >= cancelX && mx < cancelX + kBtnW) { _cancelBtn(); if (m_done) return false; }
            else if (mx >= selectX && mx < selectX + kBtnW) { _select(); if (m_done) return false; }
        }

        if (m_page == Page::Editor) {
            m_picker->handleMouseMove(mx, my);
            if (pressed)  m_picker->handleMousePress(mx, my);
            if (released) m_picker->handleMouseRelease(mx, my);
        } else if (pressed) {
            _paletteHit(mx, my);
        }

        buf.clear();
        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    enum class Page { Palette, Editor };
    struct Rc { float x, y, w, h; };

    void _select() {
        if (m_page == Page::Editor) _rememberCustom(m_working);
        if (m_onAccept) m_onAccept(m_working);
        m_done = true;
    }
    void _cancelBtn() { if (m_page == Page::Editor) { m_page = Page::Palette; return; } _cancel(); }
    void _cancel()    { if (m_onCancel) m_onCancel(); m_done = true; }

    void _rememberCustom(const std::string& hex) {
        uint8_t c[4];
        if (!JColorPicker::parseHex(hex, c)) return;   // never store "(scheme)"/inherit in customs
        auto& v = _customs();
        v.erase(std::remove(v.begin(), v.end(), hex), v.end());
        v.insert(v.begin(), hex);
        if (v.size() > static_cast<size_t>(kCols - 1)) v.resize(kCols - 1);
        _saveCustoms();
    }

    // Grid cell → colour. Columns 0..6 are hue families (light row 0 → dark row 4); column 7 is grey.
    static std::string _swatchHex(int col, int row) {
        if (col == kCols - 1) return JColorPicker::hexFromHsv(0.f, 0.f, 0.96f - row * 0.16f);
        static const float hues[7] = {210.f, 140.f, 50.f, 32.f, 0.f, 280.f, 25.f};
        const float h = hues[col] / 360.f;
        if (col == 6) return JColorPicker::hexFromHsv(h, 0.55f + row * 0.07f, 0.72f - row * 0.11f);  // brown/tan: muted
        static const float ss[5] = {0.34f, 0.52f, 0.70f, 0.85f, 1.0f};
        static const float vv[5] = {1.0f, 0.94f, 0.85f, 0.72f, 0.56f};
        return JColorPicker::hexFromHsv(h, ss[row], vv[row]);
    }

    Rc _cell(int col, int row) const {
        return { kPad + col * (kCellW + kGap), kHeader + 16.f + row * (kCellH + kGap), kCellW, kCellH };
    }
    float _customRowY() const { return kHeader + 16.f + kRows * kCellH + (kRows - 1) * kGap + 16.f + 22.f; }
    Rc _customCell(int i) const { return { kPad + i * (kCellW + kGap), _customRowY(), kCellW, kCellH }; }

    void _paletteHit(float mx, float my) {
        for (int c = 0; c < kCols; ++c)
            for (int r = 0; r < kRows; ++r) {
                const Rc cell = _cell(c, r);
                if (_in(cell, mx, my)) { m_working = _swatchHex(c, r); return; }
            }
        // Custom row: index 0 = "+", 1.. = saved customs.
        if (_in(_customCell(0), mx, my)) { m_picker->setColorHex(m_working); m_page = Page::Editor; return; }
        const auto& v = _customs();
        for (size_t i = 0; i < v.size(); ++i)
            if (_in(_customCell(static_cast<int>(i) + 1), mx, my)) { m_working = v[i]; return; }
    }

    static bool _in(const Rc& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

    void _handleDrag(float mx, float my, bool held) {
        // Drag by the header, avoiding the two buttons.
        const bool inHeader = (my >= 0.f && my < kHeader && mx > 12.f + kBtnW && mx < static_cast<float>(kW) - kBtnW - 12.f);
        if (held && inHeader && !m_dragging) { m_dragging = true; m_dragAnchorX = mx; m_dragAnchorY = my; }
        if (m_dragging) {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - static_cast<int>(m_dragAnchorX), gy - static_cast<int>(m_dragAnchorY));
        }
        if (!held) m_dragging = false;
    }

    void _render(JPrimitiveBuffer& buf, float mx, float my) {
        const float W = static_cast<float>(kW), H = static_cast<float>(m_curH);
        const float lh = JTextHelper::lineHeight();

        uint8_t bg[4] = {26, 26, 32, 255};
        buf.pushRectangle(0.f, 0.f, W, H, bg, 8.f, 1.f, Colors::Border);

        // ---- Header: Cancel | Colour | Select ----
        uint8_t tbg[4] = {38, 38, 48, 255};
        buf.pushRectangle(0.f, 0.f, W, kHeader, tbg, 8.f);
        buf.pushRectangle(0.f, 8.f, W, kHeader - 8.f, tbg, 0.f);
        const float btnY = (kHeader - kBtnH) * 0.5f;
        uint8_t btnTc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, btnTc);

        const float cancelX = 12.f;
        const bool hovCancel = (mx >= cancelX && mx < cancelX + kBtnW && my >= btnY && my < btnY + kBtnH);
        uint8_t cBg[4] = {55, 55, 65, static_cast<uint8_t>(hovCancel ? 255 : 220)};
        uint8_t cBorder[4] = {100, 100, 110, 255};
        buf.pushRectangle(cancelX, btnY, kBtnW, kBtnH, cBg, 5.f, 1.f, cBorder);
        const char* cancelLbl = (m_page == Page::Editor) ? "Cancel" : "Cancel";
        JTextHelper::pushText(buf, cancelX + (kBtnW - JTextHelper::measureWidth(cancelLbl)) * 0.5f, btnY + (kBtnH - lh) * 0.5f, cancelLbl, btnTc);

        uint8_t ttc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, ttc);
        JTextHelper::pushText(buf, (W - JTextHelper::measureWidth("Colour")) * 0.5f, (kHeader - lh) * 0.5f, "Colour", ttc, W);

        const float selectX = W - kBtnW - 12.f;
        const bool hovSel = (mx >= selectX && mx < selectX + kBtnW && my >= btnY && my < btnY + kBtnH);
        uint8_t sBg[4] = {Colors::Success[0], Colors::Success[1], Colors::Success[2], static_cast<uint8_t>(hovSel ? 255 : 230)};
        buf.pushRectangle(selectX, btnY, kBtnW, kBtnH, sBg, 5.f);
        JTextHelper::pushText(buf, selectX + (kBtnW - JTextHelper::measureWidth("Select")) * 0.5f, btnY + (kBtnH - lh) * 0.5f, "Select", btnTc);

        if (m_page == Page::Editor) { m_picker->populateRenderPrimitives(buf); return; }

        // ---- Palette grid ----
        for (int c = 0; c < kCols; ++c)
            for (int r = 0; r < kRows; ++r) {
                const Rc cell = _cell(c, r);
                const std::string hex = _swatchHex(c, r);
                _swatch(buf, cell, hex, hex == m_working);
            }

        // ---- Custom label + row ----
        uint8_t sc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sc);
        JTextHelper::pushText(buf, kPad, _customRowY() - 22.f, "Custom", sc, W);

        const Rc plus = _customCell(0);
        buf.pushRectangle(plus.x, plus.y, plus.w, plus.h, Colors::Surface2, 5.f, 1.f, Colors::Border);
        uint8_t pc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, pc);
        const float pcx = plus.x + plus.w * 0.5f, pcy = plus.y + plus.h * 0.5f;
        buf.pushRectangle(pcx - 6.f, pcy - 1.f, 12.f, 2.f, pc, 1.f);
        buf.pushRectangle(pcx - 1.f, pcy - 6.f, 2.f, 12.f, pc, 1.f);

        const auto& v = _customs();
        for (size_t i = 0; i < v.size(); ++i) {
            const Rc cell = _customCell(static_cast<int>(i) + 1);
            _swatch(buf, cell, v[i], v[i] == m_working);
        }
    }

    // A rounded swatch; draws a contrast checkmark when selected.
    static void _swatch(JPrimitiveBuffer& buf, const Rc& r, const std::string& hex, bool selected) {
        uint8_t c[4];
        if (!JColorPicker::parseHex(hex, c)) { std::copy(Colors::Surface2, Colors::Surface2 + 4, c); }
        buf.pushRectangle(r.x, r.y, r.w, r.h, c, 5.f, selected ? 2.f : 1.f, selected ? Colors::Accent : Colors::Border);
        if (!selected) return;
        const float luma = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.f;
        const JColor ink = luma < 0.5f ? jf::rgb(245, 245, 250) : jf::rgb(20, 20, 24);
        JVectorCanvas vg;
        vg.strokePolyline({ {r.x + r.w * 0.32f, r.y + r.h * 0.52f},
                            {r.x + r.w * 0.45f, r.y + r.h * 0.68f},
                            {r.x + r.w * 0.70f, r.y + r.h * 0.34f} }, 2.5f, JPaint::solid(ink));
        vg.flush(buf);
    }

    // Custom colours persist in JSettings under kCustomKey (comma-joined hex). Loaded once; if the
    // key was never written, the row seeds from the active theme's palette so it's never empty.
    static constexpr const char* kCustomKey = "ui.colorPicker.customs";

    static std::string _hexOf(const uint8_t c[4]) {
        char t[8]; std::snprintf(t, sizeof(t), "#%02x%02x%02x", c[0], c[1], c[2]); return t;
    }
    static std::vector<std::string> _themeDefaults() {
        const JTheme& t = JTheme::current();
        return { _hexOf(t.Accent), _hexOf(t.Success), _hexOf(t.Warning), _hexOf(t.Danger),
                 _hexOf(t.TextPrimary), _hexOf(t.TextSecondary), _hexOf(t.Surface3) };
    }
    static std::vector<std::string>& _customs() {
        static std::vector<std::string> v;
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            auto& s = JSettings::instance();
            if (s.has(kCustomKey)) {
                const std::string joined = s.get<std::string>(kCustomKey, "");
                for (size_t p = 0; p <= joined.size();) {
                    const size_t c = joined.find(',', p);
                    const std::string tok = joined.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (!tok.empty()) v.push_back(tok);
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            } else {
                v = _themeDefaults();
            }
        }
        return v;
    }
    static void _saveCustoms() {
        const auto& v = _customs();
        std::string joined;
        for (size_t i = 0; i < v.size(); ++i) { if (i) joined += ','; joined += v[i]; }
        auto& s = JSettings::instance();
        s.set(kCustomKey, JVariant(joined));
        s.saveJson();   // durable now if the app configured a settings path; no-op otherwise
    }

    std::function<void(std::string)> m_onAccept;
    std::function<void()>            m_onCancel;
    std::string m_working;
    Page        m_page{Page::Palette};

    JSceneGraph                      m_graph;
    std::unique_ptr<JColorPicker>    m_picker;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId                     m_surface{0};
    uint32_t m_curH{0};
    bool  m_done{false};
    bool  m_dragging{false};
    float m_dragAnchorX{0.f}, m_dragAnchorY{0.f};
};

} // inline namespace jf
