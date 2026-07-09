#pragma once

// JFontPickerDialog — the font chooser JFontButton opens. A WM-managed modal (own surface, title bar,
// draggable) matching the original studio's Select-Font dialog: a search field, a scrollable family
// LIST (not a dropdown — nothing to mis-position), a live preview line, and a size spin. The chosen
// font is applied only on Select (onAccept), as a compact spec "family|size|b|i".
// NOTE: the text engine currently renders one atlas font, so family/bold/italic are stored + shown but
// not yet visually distinct — extending the text engine is the render-side follow-up.

#include <j/core/JSpinBox.h>
#include <j/core/JSlider.h>
#include <j/core/JListView.h>
#include <j/core/JLineEdit.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/FontEngine.h>       // jListSystemFonts() — REAL installed fonts, no external toolkit

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
        // REAL installed fonts (family name + file path), enumerated in-process — never a hardcoded stub
        // and never an external chooser. "Default" heads the list to mean "the framework's built-in face".
        m_systemFonts = jListSystemFonts();
        m_allFamilies.clear();
        m_allFamilies.push_back("Default");
        for (const auto& sf : m_systemFonts) m_allFamilies.push_back(sf.name);

        m_search = std::make_unique<JLineEdit>(m_graph, "Search fonts…");
        m_list   = std::make_unique<JListView>(m_graph, m_allFamilies);
        m_size   = std::make_unique<JSpinBox>(m_graph, 6, 96, 96.f, kRowH); m_size->setValue(std::atoi(sz.c_str()));
        m_sizeSlider = std::make_unique<JSlider>(m_graph);
        m_sizeSlider->setValue(_sizeToNorm(m_size->value()));
        for (size_t i = 0; i < m_allFamilies.size(); ++i) if (m_allFamilies[i] == fam) m_list->setSelectedIndex(static_cast<int>(i));
        // Slider (0..1) and spin (6..96 px) mirror one size; a guard stops the mutual setValue from looping.
        m_sizeSlider->onValueChanged.connect([this](float v) { if (m_syncing) return; m_syncing = true; m_size->setValue(_normToSize(v)); m_syncing = false; });
        m_size->onValueChanged.connect([this](int s) { if (m_syncing) return; m_syncing = true; m_sizeSlider->setValue(_sizeToNorm(s)); m_syncing = false; });

        // Search filters the family list (keeping the current pick selected if it survives the filter).
        // NB: JListView emits onItemActivated on a single click, so a click only SELECTS here — the pick is
        // confirmed with Select / Enter (wiring activation to close would dismiss the dialog on any click).
        m_search->onTextChanged.connect([this](const std::string& q) { _applyFilter(q); });
    }

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    // Enable APP-FONT mode: the picker drives the whole-app atlas live. onPreview fires as the selection
    // changes (apply that font file now, so the entire app — and this dialog, sharing the atlas — re-renders
    // in it); onCancel reverts to the original; Select commits the chosen FILE PATH (empty = built-in
    // "Default"). Seed the list to the current family via startSpec so it opens on the active font.
    void setAppFontMode(std::function<void(std::string)> onPreview,
                        std::function<void(std::string)> onAcceptPath,
                        std::function<void()>            onCancel) {
        m_onPreview    = std::move(onPreview);
        m_onAcceptPath = std::move(onAcceptPath);
        m_onCancelCb   = std::move(onCancel);
        m_previewed    = _selectedFamily();          // don't re-apply the already-active font on open
    }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { _cancel(); return false; }
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
            if (ke.key == JKeyEvent::JKey::Escape) { _cancel(); return false; }
            if (ke.key == JKeyEvent::JKey::Return) { _accept(); return false; }
            m_search->handleKeyEvent(ke);   // typing filters the list
        }

        // Lay the widgets out, then route the mouse to them.
        const float listY = kHeader + kRowH + 2.f * kPad, sizeY = kH - kRowH - kPad, prevY = sizeY - kRowH - 8.f;
        m_search->setBounds({ kPad, kHeader + kPad, static_cast<float>(kW) - 2.f * kPad, kRowH });
        m_list->setBounds({ kPad, listY, static_cast<float>(kW) - 2.f * kPad, prevY - listY - 8.f });
        const float spinX = static_cast<float>(kW) - kPad - 96.f, slX = kPad + 44.f, slW = spinX - 8.f - slX;
        m_size->setBounds({ spinX, sizeY, 96.f, kRowH });
        m_sizeSlider->setBounds({ slX, sizeY + (kRowH - 24.f) * 0.5f, slW, 24.f });
        m_search->handleMouseMove(mx, my); m_list->handleMouseMove(mx, my); m_size->handleMouseMove(mx, my); m_sizeSlider->handleMouseMove(mx, my);
        if (pressed)  { m_search->handleMousePress(mx, my);   m_list->handleMousePress(mx, my);   m_size->handleMousePress(mx, my);   m_sizeSlider->handleMousePress(mx, my); }
        if (released) { m_search->handleMouseRelease(mx, my); m_list->handleMouseRelease(mx, my); m_size->handleMouseRelease(mx, my); m_sizeSlider->handleMouseRelease(mx, my); }

        // App-font mode: live-preview the selection by applying it now — the whole app (and this dialog,
        // which shares the atlas) instantly re-renders in the chosen face. Only on an actual change.
        if (m_onPreview) {
            const std::string sel = _selectedFamily();
            if (sel != m_previewed) { m_previewed = sel; m_onPreview(_pathForFamily(sel)); }
        }

        // Header buttons.
        if (pressed && my >= 8.f && my < 8.f + kBtnH) {
            if (mx >= okX && mx < okX + kBtnW)         { _accept(); return false; }
            if (mx >= cancelX && mx < cancelX + kBtnW) { _cancel(); return false; }
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
    static float _sizeToNorm(int px) { return (std::clamp(px, 6, 96) - 6) / 90.f; }   // 6..96 px -> 0..1
    static int   _normToSize(float v) { return 6 + static_cast<int>(std::clamp(v, 0.f, 1.f) * 90.f + 0.5f); }
    std::string _pathForFamily(const std::string& fam) const {
        if (fam == "Default") return {};                       // "" = the framework's built-in face
        for (const auto& sf : m_systemFonts) if (sf.name == fam) return sf.path;
        return {};
    }
    void _accept() {
        if (m_onAcceptPath) { m_onAcceptPath(_pathForFamily(_selectedFamily())); return; }   // app-font mode: return the FILE PATH
        const std::string fam = _selectedFamily();
        const std::string famSpec = (fam == "Default") ? std::string() : fam;   // Default = ""
        std::string spec = famSpec + "|" + std::to_string(m_size->value()) + "|" + (m_bold ? "1" : "0") + "|" + (m_ital ? "1" : "0");
        if (m_onAccept) m_onAccept(spec);
    }
    void _cancel() { if (m_onCancelCb) m_onCancelCb(); }        // app-font mode: revert the live preview
    void _render(JPrimitiveBuffer& buf, float mx, float my, float okX, float cancelX, float prevY) {
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH), lh = JTextHelper::lineHeight();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        buf.pushRectangle(0.f, 0.f, W, kHeader, Colors::DialogTitleBg, 8.f); buf.pushRectangle(0.f, 8.f, W, kHeader - 8.f, Colors::DialogTitleBg, 0.f);
        uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);

        // Cancel (left) + Select (right, green) in the header, title centred between.
        uint8_t cBg[4] = {Colors::CancelBtnBg[0], Colors::CancelBtnBg[1], Colors::CancelBtnBg[2], 230};
        buf.pushRectangle(cancelX, 8.f, kBtnW, kBtnH, cBg, 5.f, 1.f, Colors::CancelBtnBorder);
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
        buf.pushRectangle(kPad, prevY, W - 2.f * kPad, kRowH, Colors::PreviewBg, 5.f, 1.f, Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t sub[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sub);
            JTextHelper::pushText(buf, kPad + 8.f, prevY + (kRowH - lh) * 0.5f, "The quick brown fox jumps over the lazy dog.", sub, W - 2.f * kPad - 16.f);
            JTextHelper::pushText(buf, kPad, (H - kRowH - kPad) + (kRowH - lh) * 0.5f, "Size", tc);
        }
        m_sizeSlider->populateRenderPrimitives(buf);
        m_size->populateRenderPrimitives(buf);
    }

    std::function<void(std::string)> m_onAccept;
    // App-font mode (empty unless setAppFontMode() was called): live preview + path-returning accept + revert.
    std::function<void(std::string)> m_onPreview, m_onAcceptPath;
    std::function<void()>            m_onCancelCb;
    std::string                      m_previewed;                 // last family we live-applied (dedupe)
    std::vector<JSystemFont>         m_systemFonts;               // real installed fonts (name -> path)
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{0};
    JSceneGraph  m_graph;
    // Owned, and declared AFTER m_graph so they're destroyed BEFORE it — each JWidget then deregisters from
    // JWidget::s_activeWidgets while its scene graph is still alive, so nothing dangles for the AI snapshot.
    std::unique_ptr<JLineEdit> m_search;
    std::unique_ptr<JListView> m_list;
    std::unique_ptr<JSlider>   m_sizeSlider;
    std::unique_ptr<JSpinBox>  m_size;
    std::vector<std::string>   m_allFamilies;
    bool m_syncing{false};   // re-entrancy guard for the slider <-> spin two-way sync
    bool m_bold{false}, m_ital{false};
    bool m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};
};

} // inline namespace jf
