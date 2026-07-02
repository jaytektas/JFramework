#pragma once

// JFontPickerDialog — the font chooser JFontButton opens. A WM-managed modal (own surface, title bar,
// draggable) with a family list, size, bold/italic, a preview, and OK/Cancel. The chosen font is
// applied only on OK (onAccept), as a compact spec "family|size|b|i". Mouse-driven (combo/spin/checks).
// NOTE: the text engine currently renders one atlas font, so family/bold/italic are stored + shown but
// not yet visually distinct — extending the text engine is the render-side follow-up.

#include <j/core/BaseWidgets.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JFontPickerDialog {
public:
    static constexpr uint32_t kW = 320, kH = 250;
    static constexpr float kHeader = 38.f, kBtnW = 84.f, kBtnH = 30.f, kRowH = 28.f, kPad = 14.f;

#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JFontPickerDialog(const std::string& startSpec, JGpuHal& hal, int sx, int sy, NativeWinHandleType parent,
                      std::function<void(std::string)> onAccept)
        : m_onAccept(std::move(onAccept))
        , m_window(std::make_unique<PlatformWinType>("Font", kW, kH, sx, sy, JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH))
        , m_form(m_graph) {
        m_form.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ kPad, kHeader + 8.f, kPad, kPad });
        // Parse "family|size|b|i".
        std::string fam = "Default", sz = "12"; bool bold = false, ital = false;
        { size_t p = 0; int f = 0; while (p <= startSpec.size()) { const size_t c = startSpec.find('|', p);
            const std::string t = startSpec.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (f == 0 && !t.empty()) fam = t; else if (f == 1 && !t.empty()) sz = t; else if (f == 2) bold = (t == "1"); else if (f == 3) ital = (t == "1");
            ++f; if (c == std::string::npos) break; p = c + 1; } }
        m_families = { "Default", "Sans", "Serif", "Mono", "Ubuntu", "DejaVu Sans", "DejaVu Serif" };
        m_family = new JComboBox(m_graph, m_families, 150.f, kRowH);
        for (size_t i = 0; i < m_families.size(); ++i) if (m_families[i] == fam) m_family->setCurrentIndex(static_cast<int>(i));
        m_size = new JSpinBox(m_graph, 6, 96, 90.f, kRowH); m_size->setValue(std::atoi(sz.c_str()));
        m_bold = new JCheckBox(m_graph, "", 90.f, kRowH);   m_bold->setChecked(bold);
        m_ital = new JCheckBox(m_graph, "", 90.f, kRowH);   m_ital->setChecked(ital);
        auto row = [&](const char* cap, JWidget* ed) { m_caps.push_back(std::make_unique<JLabel>(m_graph, cap, 90.f, kRowH)); m_form.add(m_caps.back().get()); m_form.add(ed); };
        row("Family", m_family); row("Size", m_size); row("Bold", m_bold); row("Italic", m_ital);
    }

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();
        const bool inTitle = (my >= 0.f && my < kHeader && mx < kW - kBtnW - 12.f);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;
        for (const auto& ke : m_window->consumeAllKeys()) if (ke.pressed && ke.key == JKeyEvent::JKey::Escape) return false;

        m_form.setBounds({ 0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH) });
        m_form.handleMouseMove(mx, my);
        if (pressed)  m_form.handleMousePress(mx, my);
        if (released) m_form.handleMouseRelease(mx, my);
        const float btnY = kH - kBtnH - 12.f, okX = kW - kBtnW - 12.f, cancelX = kW - kBtnW * 2.f - 20.f;
        if (pressed && my >= btnY && my < btnY + kBtnH) {
            if (mx >= okX && mx < okX + kBtnW) { _accept(); return false; }
            if (mx >= cancelX && mx < cancelX + kBtnW) return false;
        }

        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        uint8_t bg[4] = {26, 26, 32, 255}; buf.pushRectangle(0.f, 0.f, W, H, bg, 8.f, 1.f, Colors::Border);
        uint8_t tbg[4] = {38, 38, 48, 255}; buf.pushRectangle(0.f, 0.f, W, kHeader, tbg, 8.f); buf.pushRectangle(0.f, 8.f, W, kHeader - 8.f, tbg, 0.f);
        const float lh = JTextHelper::lineHeight();
        uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
        if (JTextHelper::hasAtlas()) JTextHelper::pushText(buf, 14.f, (kHeader - lh) * 0.5f, "Font", tc, W - 20.f);
        m_form.populateRenderPrimitives(buf);
        const bool hovOk = (mx >= okX && mx < okX + kBtnW && my >= btnY && my < btnY + kBtnH);
        uint8_t okBg[4] = {Colors::Success[0], Colors::Success[1], Colors::Success[2], static_cast<uint8_t>(hovOk ? 255 : 230)};
        buf.pushRectangle(okX, btnY, kBtnW, kBtnH, okBg, 5.f);
        if (JTextHelper::hasAtlas()) JTextHelper::pushText(buf, okX + (kBtnW - JTextHelper::measureWidth("OK")) * 0.5f, btnY + (kBtnH - lh) * 0.5f, "OK", tc);
        uint8_t cBg[4] = {55, 55, 65, 230}, cBorder[4] = {100, 100, 110, 255};
        buf.pushRectangle(cancelX, btnY, kBtnW, kBtnH, cBg, 5.f, 1.f, cBorder);
        if (JTextHelper::hasAtlas()) JTextHelper::pushText(buf, cancelX + (kBtnW - JTextHelper::measureWidth("Cancel")) * 0.5f, btnY + (kBtnH - lh) * 0.5f, "Cancel", tc);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void _accept() {
        const int fi = m_family->currentIndex();
        const std::string fam = (fi > 0 && fi < static_cast<int>(m_families.size())) ? m_families[fi] : std::string();   // index 0 = Default = ""
        std::string spec = fam + "|" + std::to_string(m_size->value()) + "|" + (m_bold->isChecked() ? "1" : "0") + "|" + (m_ital->isChecked() ? "1" : "0");
        if (m_onAccept) m_onAccept(spec);
    }

    std::function<void(std::string)> m_onAccept;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{0};
    JSceneGraph  m_graph;
    JContainer   m_form;
    std::vector<std::unique_ptr<JLabel>> m_caps;
    std::vector<std::string> m_families;
    JComboBox* m_family{}; JSpinBox* m_size{}; JCheckBox* m_bold{}; JCheckBox* m_ital{};
    bool m_drag{false}; float m_ax{0}, m_ay{0};
};

} // inline namespace jf
