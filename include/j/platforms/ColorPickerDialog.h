#pragma once

// ============================================================================
// JColorPickerDialog — the colour chooser as a modal dialog window, matching the studio's picker.
//
// Presents as a normal dialog: a JStyle title bar (the same JTitleBar every other window uses) with the
// canonical close button, and a bottom OK/Cancel bar (mirroring JNativeDialogWindow). Two pages:
//   • Palette  — a hue×shade grid of preset swatches (checkmark on the current one) plus a "Custom"
//                row of previously-defined colours and a "+" that opens the editor.
//   • Editor   — the "+" custom page: the JColorPicker (hue bar, SV square, eyedropper, hex, alpha).
//
// A real WM-managed window (keyboard focus, no pointer grab, draggable by the title bar) — it does not
// dismiss on click-outside. Select applies the working colour (onAccept); Cancel/close/Escape discard.
// On the editor page Cancel returns to the palette; Select applies + remembers the custom.
// ============================================================================

#include <j/core/ColorPicker.h>
#include <j/core/JTitleBar.h>       // shared styled title bar (JStyle) — same one every other window uses
#include <j/core/JCloseButton.h>    // canonical window close control
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
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JColorPickerDialog {
public:
    static constexpr uint32_t kW = 452;
    static constexpr float    kFooterH = 58.f;                 // bottom OK/Cancel button bar
    static constexpr float    kDlgBtnW = 88.f;                            // button WIDTH (JStyle has no button-width metric)
    static float kDlgBtnH() { return JStyle::current().buttonHeight; }    // HEIGHT from JStyle — single source of truth
    // Palette-page grid.
    static constexpr int   kCols = 8, kRows = 5;
    static constexpr float kCellW = 48.f, kCellH = 32.f, kGap = 6.f, kPad = 13.f;
    static float hdrH()       { return JStyle::current().titleBarHeight; }   // title bar height from the theme
    static float btnBarY(float H) { return H - kDlgBtnH() - 14.f; }
    static float selBtnX()   { return static_cast<float>(kW) - kDlgBtnW - 12.f; }        // Select (accent), rightmost
    static float cancelBtnX(){ return static_cast<float>(kW) - kDlgBtnW * 2.f - 20.f; }  // Cancel, to its left
    // Window heights per page (runtime — the title bar height comes from the theme). Include the footer.
    static float paletteH() { return hdrH() + 16.f + (kRows * kCellH + (kRows - 1) * kGap) + 16.f + 22.f + kCellH + 14.f + kFooterH; }
    static float editorH()  { return hdrH() + 10.f + JColorPicker::kH + 14.f + kFooterH; }

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
        , m_window(std::make_unique<PlatformWinType>("Colour", kW, static_cast<uint32_t>(paletteH()), screenX, screenY,
                                                     JPlatformWindowStyle::Borderless, parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, static_cast<uint32_t>(paletteH())))
        , m_curH(static_cast<uint32_t>(paletteH()))
    {
        m_picker = std::make_unique<JColorPicker>(m_graph);
        auto& l = m_graph.getLayout(m_picker->getNodeId());
        l.boundingBox.x = (kW - JColorPicker::kW) * 0.5f;
        l.boundingBox.y = hdrH() + 10.f;
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
        const uint32_t wantH = static_cast<uint32_t>(m_page == Page::Editor ? editorH() : paletteH());
        if (wantH != m_curH) { m_curH = wantH; m_window->setSize(kW, wantH); hal.resizeSurface(m_surface, kW, wantH); }

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed  = m_window->consumePress();
        const bool released = m_window->consumeRelease();
        const bool held     = m_window->isLeftButtonDown();

        // Title-bar drag (the whole bar except the close button); the close button cancels.
        const JRect closeR = JCloseButton::rectFor({ 0.f, 0.f, static_cast<float>(kW), hdrH() });
        _handleDrag(mx, my, held, closeR.x);
        if (pressed && JCloseButton::hit(closeR, mx, my)) { _cancel(); return false; }

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) { if (m_page == Page::Editor) { m_page = Page::Palette; continue; } _cancel(); return false; }
            if (m_page == Page::Editor && m_picker->handleKeyEvent(ke)) continue;
            if (ke.key == K::Return) { _select(); if (m_done) return false; continue; }
        }

        // Footer buttons (bottom of the dialog): Select (accent) and Cancel.
        const float H = static_cast<float>(m_curH);
        const float by = btnBarY(H);
        bool footerHit = false;
        if (pressed && my >= by && my < by + kDlgBtnH()) {
            if      (mx >= cancelBtnX() && mx < cancelBtnX() + kDlgBtnW) { footerHit = true; _cancelBtn(); if (m_done) return false; }
            else if (mx >= selBtnX()    && mx < selBtnX()    + kDlgBtnW) { footerHit = true; _select();    if (m_done) return false; }
        }

        if (m_page == Page::Editor) {
            m_picker->handleMouseMove(mx, my);
            if (pressed && !footerHit) m_picker->handleMousePress(mx, my);
            if (released)              m_picker->handleMouseRelease(mx, my);
        } else if (pressed && !footerHit) {
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
        return { kPad + col * (kCellW + kGap), hdrH() + 16.f + row * (kCellH + kGap), kCellW, kCellH };
    }
    float _customRowY() const { return hdrH() + 16.f + kRows * kCellH + (kRows - 1) * kGap + 16.f + 22.f; }
    Rc _customCell(int i) const { return { kPad + i * (kCellW + kGap), _customRowY(), kCellW, kCellH }; }

    void _paletteHit(float mx, float my) {
        for (int c = 0; c < kCols; ++c)
            for (int r = 0; r < kRows; ++r)
                if (_in(_cell(c, r), mx, my)) { _pick(_swatchHex(c, r)); return; }
        // Custom row: index 0 = "+", 1.. = saved customs.
        if (_in(_customCell(0), mx, my)) { m_picker->setColorHex(m_working); m_page = Page::Editor; return; }
        const auto& v = _customs();
        for (size_t i = 0; i < v.size(); ++i)
            if (_in(_customCell(static_cast<int>(i) + 1), mx, my)) { _pick(v[i]); return; }
    }

    // Select a swatch. A SECOND click on the same swatch within the double-click window commits it (as OK).
    void _pick(const std::string& hex) {
        const int64_t now = _nowMs();
        const bool dbl = (hex == m_lastClickHex && now - m_lastClickMs < 400);
        m_working = hex;
        m_lastClickHex = hex; m_lastClickMs = now;
        if (dbl) _select();
    }
    static int64_t _nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static bool _in(const Rc& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

    void _handleDrag(float mx, float my, bool held, float closeX) {
        const bool inTitle = (my >= 0.f && my < hdrH() && mx < closeX);
        if (held && inTitle && !m_dragging) { m_dragging = true; m_dragAnchorX = mx; m_dragAnchorY = my; }
        if (m_dragging) {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - static_cast<int>(m_dragAnchorX), gy - static_cast<int>(m_dragAnchorY));
        }
        if (!held) m_dragging = false;
    }

    // Footer bar — Cancel + Select, mirroring JNativeDialogWindow's layout.
    void _footer(JPrimitiveBuffer& buf, float mx, float my, float H) const {
        const float lh = JTextHelper::lineHeight();
        const float by = btnBarY(H), okX = selBtnX(), caX = cancelBtnX();
        uint8_t cBg[4] = { Colors::CancelBtnBg[0], Colors::CancelBtnBg[1], Colors::CancelBtnBg[2],
                           static_cast<uint8_t>((mx >= caX && mx < caX + kDlgBtnW && my >= by && my < by + kDlgBtnH()) ? 255 : 220) };
        buf.pushRectangle(caX, by, kDlgBtnW, kDlgBtnH(), cBg, JStyle::current().cornerRadius, 1.f, Colors::CancelBtnBorder);
        const bool hovOk = (mx >= okX && mx < okX + kDlgBtnW && my >= by && my < by + kDlgBtnH());
        uint8_t okBg[4] = { Colors::PrimaryBtnBg[0], Colors::PrimaryBtnBg[1], Colors::PrimaryBtnBg[2], static_cast<uint8_t>(hovOk ? 255 : 220) };
        buf.pushRectangle(okX, by, kDlgBtnW, kDlgBtnH(), okBg, JStyle::current().cornerRadius, 1.f, Colors::PrimaryBtnBorder);
        if (JTextHelper::hasAtlas()) {
            JTextHelper::pushText(buf, caX + (kDlgBtnW - JTextHelper::measureWidth("Cancel")) * 0.5f, by + (kDlgBtnH() - lh) * 0.5f, "Cancel", Colors::CancelBtnText);
            JTextHelper::pushText(buf, okX + (kDlgBtnW - JTextHelper::measureWidth("Select")) * 0.5f, by + (kDlgBtnH() - lh) * 0.5f, "Select", Colors::PrimaryBtnText);
        }
    }

    void _render(JPrimitiveBuffer& buf, float mx, float my) {
        const float W = static_cast<float>(kW), H = static_cast<float>(m_curH);

        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);

        // Title bar — the SAME styled bar every other window uses (JStyle), with the canonical close button.
        const JRect closeR = JCloseButton::rectFor({ 0.f, 0.f, W, hdrH() });
        JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Colour", JStyle::current().cornerRadius, 0, 12.f, closeR.width + 14.f);
        JCloseButton::draw(buf, closeR, JCloseButton::hit(closeR, mx, my));

        if (m_page == Page::Editor) { m_picker->populateRenderPrimitives(buf); _footer(buf, mx, my, H); return; }

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

        _footer(buf, mx, my, H);
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
        const JStyle& t = JStyle::current();
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
    std::string m_lastClickHex;   // last swatch clicked + when — a repeat within the window = double-click commit
    int64_t     m_lastClickMs{0};
};

} // inline namespace jf
