#pragma once

// JFontPickerDialog — the font chooser JFontButton opens. A WM-managed modal (own surface, title bar,
// draggable) matching the original studio's Select-Font dialog: a search field, a scrollable family
// LIST (not a dropdown — nothing to mis-position), a live preview line, and a size spin. The chosen
// font is applied only on Select (onAccept), as a compact spec "family|size|b|i".
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

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JFontPickerDialog {
public:
    static constexpr uint32_t kW = 380, kH = 470;
    static constexpr float kHeader = 44.f, kBtnW = 84.f, kBtnH = 28.f, kPad = 12.f, kRowH = 30.f;

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
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        // Parse "family|size|b|i".
        std::string fam = "Default", sz = "12";
        { size_t p = 0; int f = 0; while (p <= startSpec.size()) { const size_t c = startSpec.find('|', p);
            const std::string t = startSpec.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (f == 0 && !t.empty()) fam = t; else if (f == 1 && !t.empty()) sz = t; else if (f == 2) m_bold = (t == "1"); else if (f == 3) m_ital = (t == "1");
            ++f; if (c == std::string::npos) break; p = c + 1; } }
        m_allFamilies = { "Default", "Sans", "Sans Bold", "Serif", "Serif Bold", "Mono", "Mono Bold",
                          "Ubuntu", "Ubuntu Light", "Ubuntu Medium", "DejaVu Sans", "DejaVu Sans Mono", "DejaVu Serif" };

        m_search = std::make_unique<JLineEdit>(m_graph, "Search fonts…");
        m_list   = std::make_unique<JListView>(m_graph, m_allFamilies);
        m_size   = std::make_unique<JSpinBox>(m_graph, 6, 96, 96.f, kRowH); m_size->setValue(std::atoi(sz.c_str()));
        for (size_t i = 0; i < m_allFamilies.size(); ++i) if (m_allFamilies[i] == fam) m_list->setSelectedIndex(static_cast<int>(i));

        // Search filters the family list (keeping the current pick selected if it survives the filter).
        // NB: JListView emits onItemActivated on a single click, so a click only SELECTS here — the pick is
        // confirmed with Select / Enter (wiring activation to close would dismiss the dialog on any click).
        m_search->onTextChanged.connect([this](const std::string& q) { _applyFilter(q); });
    }

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        const float okX = kW - kBtnW - kPad, cancelX = kPad;
        // Title-bar drag (between the two header buttons).
        const bool inTitle = (my >= 0.f && my < kHeader && mx >= cancelX + kBtnW && mx < okX);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (ke.key == JKeyEvent::JKey::Escape) return false;
            if (ke.key == JKeyEvent::JKey::Return) { _accept(); return false; }
            m_search->handleKeyEvent(ke);   // typing filters the list
        }

        // Lay the widgets out, then route the mouse to them.
        const float listY = kHeader + kRowH + 2.f * kPad, sizeY = kH - kRowH - kPad, prevY = sizeY - kRowH - 8.f;
        m_search->setBounds({ kPad, kHeader + kPad, static_cast<float>(kW) - 2.f * kPad, kRowH });
        m_list->setBounds({ kPad, listY, static_cast<float>(kW) - 2.f * kPad, prevY - listY - 8.f });
        m_size->setBounds({ static_cast<float>(kW) - kPad - 96.f, sizeY, 96.f, kRowH });
        m_search->handleMouseMove(mx, my); m_list->handleMouseMove(mx, my); m_size->handleMouseMove(mx, my);
        if (pressed)  { m_search->handleMousePress(mx, my);   m_list->handleMousePress(mx, my);   m_size->handleMousePress(mx, my); }
        if (released) { m_search->handleMouseRelease(mx, my); m_list->handleMouseRelease(mx, my); m_size->handleMouseRelease(mx, my); }

        // Header buttons.
        if (pressed && my >= 8.f && my < 8.f + kBtnH) {
            if (mx >= okX && mx < okX + kBtnW)         { _accept(); return false; }
            if (mx >= cancelX && mx < cancelX + kBtnW) return false;
        }

        _render(buf, mx, my, okX, cancelX, prevY);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return !m_done;
    }

private:
    void _applyFilter(const std::string& q) {
        std::string lq; for (char c : q) lq += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string keep = _selectedFamily();
        std::vector<std::string> out;
        for (const auto& f : m_allFamilies) {
            std::string lf; for (char c : f) lf += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lq.empty() || lf.find(lq) != std::string::npos) out.push_back(f);
        }
        m_list->setItems(out);
        for (size_t i = 0; i < out.size(); ++i) if (out[i] == keep) { m_list->setSelectedIndex(static_cast<int>(i)); break; }
    }
    std::string _selectedFamily() const {
        const int i = m_list->selectedIndex();
        return (i >= 0 && i < static_cast<int>(m_list->items().size())) ? m_list->items()[i] : std::string("Default");
    }
    void _accept() {
        const std::string fam = _selectedFamily();
        const std::string famSpec = (fam == "Default") ? std::string() : fam;   // Default = ""
        std::string spec = famSpec + "|" + std::to_string(m_size->value()) + "|" + (m_bold ? "1" : "0") + "|" + (m_ital ? "1" : "0");
        if (m_onAccept) m_onAccept(spec);
    }
    void _render(JPrimitiveBuffer& buf, float mx, float my, float okX, float cancelX, float prevY) {
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH), lh = JTextHelper::lineHeight();
        uint8_t bg[4] = {26, 26, 32, 255}; buf.pushRectangle(0.f, 0.f, W, H, bg, 8.f, 1.f, Colors::Border);
        uint8_t tbg[4] = {38, 38, 48, 255}; buf.pushRectangle(0.f, 0.f, W, kHeader, tbg, 8.f); buf.pushRectangle(0.f, 8.f, W, kHeader - 8.f, tbg, 0.f);
        uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);

        // Cancel (left) + Select (right, green) in the header, title centred between.
        uint8_t cBg[4] = {55, 55, 65, 230}, cBorder[4] = {100, 100, 110, 255};
        buf.pushRectangle(cancelX, 8.f, kBtnW, kBtnH, cBg, 5.f, 1.f, cBorder);
        const bool hovOk = (mx >= okX && mx < okX + kBtnW && my >= 8.f && my < 8.f + kBtnH);
        uint8_t okBg[4] = {Colors::Success[0], Colors::Success[1], Colors::Success[2], static_cast<uint8_t>(hovOk ? 255 : 230)};
        buf.pushRectangle(okX, 8.f, kBtnW, kBtnH, okBg, 5.f);
        if (JTextHelper::hasAtlas()) {
            JTextHelper::pushText(buf, cancelX + (kBtnW - JTextHelper::measureWidth("Cancel")) * 0.5f, 8.f + (kBtnH - lh) * 0.5f, "Cancel", tc);
            JTextHelper::pushText(buf, okX + (kBtnW - JTextHelper::measureWidth("Select")) * 0.5f, 8.f + (kBtnH - lh) * 0.5f, "Select", tc);
            JTextHelper::pushText(buf, (W - JTextHelper::measureWidth("Select Font")) * 0.5f, (kHeader - lh) * 0.5f, "Select Font", tc);
        }

        m_search->populateRenderPrimitives(buf);
        m_list->populateRenderPrimitives(buf);

        // Preview line + size row.
        uint8_t pbg[4] = {20, 20, 26, 255}; buf.pushRectangle(kPad, prevY, W - 2.f * kPad, kRowH, pbg, 5.f, 1.f, Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t sub[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sub);
            JTextHelper::pushText(buf, kPad + 8.f, prevY + (kRowH - lh) * 0.5f, "The quick brown fox jumps over the lazy dog.", sub, W - 2.f * kPad - 16.f);
            JTextHelper::pushText(buf, kPad, (H - kRowH - kPad) + (kRowH - lh) * 0.5f, "Size", tc);
        }
        m_size->populateRenderPrimitives(buf);
    }

    std::function<void(std::string)> m_onAccept;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{0};
    JSceneGraph  m_graph;
    // Owned, and declared AFTER m_graph so they're destroyed BEFORE it — each JWidget then deregisters from
    // JWidget::s_activeWidgets while its scene graph is still alive, so nothing dangles for the AI snapshot.
    std::unique_ptr<JLineEdit> m_search;
    std::unique_ptr<JListView> m_list;
    std::unique_ptr<JSpinBox>  m_size;
    std::vector<std::string>   m_allFamilies;
    bool m_bold{false}, m_ital{false};
    bool m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};
};

} // inline namespace jf
