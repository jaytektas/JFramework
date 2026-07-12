#pragma once

// JFontPickerDialog — the font chooser JFontButton opens. A fixed-size, WM-managed modal presenting as a
// normal dialog: a JStyle title bar (the same JTitleBar every other window uses) with the canonical close
// button, a search field, a scrollable family LIST, a live preview line that renders in the highlighted
// face at the chosen size, a size slider marked with common sizes + a size spin, and a bottom OK/Cancel bar
// (mirroring JNativeDialogWindow's chrome). The chosen font is applied only on Select (onAccept), as a
// compact spec "family|size|b|i". In app-font mode (setAppFontMode) Select returns the FILE PATH + size px.
// NOTE: browsing does NOT change the app font — only the preview line renders the chosen face (its own atlas).

#include <j/core/JSpinBox.h>
#include <j/core/JSlider.h>
#include <j/core/JListView.h>
#include <j/core/JLineEdit.h>
#include <j/core/JTitleBar.h>       // shared styled title bar (JStyle) — same one every other window uses
#include <j/core/JCloseButton.h>    // canonical window close control
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
    static constexpr float kPad = 12.f, kRowH = 30.f;
    static constexpr float kSizeRowH = 48.f;    // size row: slider + a tick/label strip of common sizes beneath it
    static constexpr float kFooterH  = 58.f;    // bottom OK/Cancel button bar — a normal dialog, buttons at the bottom
    static constexpr float kDlgBtnW = 88.f;                    // footer button WIDTH (JStyle has no button-width metric)
    static float kDlgBtnH() { return JStyle::current().buttonHeight; }   // HEIGHT from JStyle — single source of truth
    static float hdrH()      { return JStyle::current().titleBarHeight; }              // title bar height from the theme
    static float btnBarY()   { return static_cast<float>(kH) - kDlgBtnH() - 14.f; }
    static float selBtnX()   { return static_cast<float>(kW) - kDlgBtnW - 12.f; }        // Select (accent), rightmost
    static float cancelBtnX(){ return static_cast<float>(kW) - kDlgBtnW * 2.f - 20.f; }  // Cancel, to its left
    // Common point sizes marked on the size slider. Every entry gets a tick; the sparser labelled subset
    // also shows the number (the slider is linear in px, so the small sizes crowd — don't label them all).
    static constexpr int kTickSizes[]  = { 8, 10, 12, 14, 16, 18, 24, 36, 48, 60, 72, 96 };
    static constexpr int kLabelSizes[] = { 12, 24, 48, 72, 96 };

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

    void destroySurface(JGpuHal& hal) {
        if (m_prevId) hal.freeFontAtlas(m_prevId);   // release the preview line's own atlas
        m_prevId = 0;
        hal.destroySurface(m_surface);
    }

    // Enable APP-FONT mode: Select commits the chosen FILE PATH (empty = built-in "Default") + size px via
    // onAcceptPath, so the caller applies it to the WHOLE app only THEN. Browsing never changes the app font
    // — only the preview line renders the highlighted face. Seed the list via startSpec so it opens on it.
    void setAppFontMode(std::function<void(std::string,int)> onAcceptPath) {
        m_onAcceptPath = std::move(onAcceptPath);
    }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { _cancel(); return false; }
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        const float hdr = hdrH();
        const JRect closeR = JCloseButton::rectFor({ 0.f, 0.f, static_cast<float>(kW), hdr });
        // Title-bar drag (the whole bar except the close button); the close button cancels.
        const bool inTitle = (my >= 0.f && my < hdr && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;
        if (pressed && JCloseButton::hit(closeR, mx, my)) { _cancel(); return false; }

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) { _cancel(); return false; }
            if (ke.key == K::Return) { _accept(); return false; }
            if (ke.key == K::Up || ke.key == K::Down || ke.key == K::PageUp ||
                ke.key == K::PageDown || ke.key == K::Home || ke.key == K::End) {
                m_list->handleKeyEvent(ke); continue;   // arrow/paging keys navigate the family list
            }
            m_search->handleKeyEvent(ke);   // typing filters the list
        }

        // Bake the highlighted family (at the chosen size) into a LOCAL atlas so ONLY the preview line
        // renders in it — the app font is untouched until Select. Rebuilds only when family/size change.
        _refreshPreview(hal);

        // Fixed layout: search / list / preview box / size row, above the footer button bar.
        const float W = static_cast<float>(kW);
        const float sizeY = kH - kFooterH - kSizeRowH - kPad;
        const float listY = hdr + kRowH + 2.f * kPad;
        const float prevH = _previewH();
        const float prevY = sizeY - prevH - 8.f;
        m_search->setBounds({ kPad, hdr + kPad, W - 2.f * kPad, kRowH });
        m_list->setBounds({ kPad, listY, W - 2.f * kPad, prevY - listY - 8.f });
        const float spinX = W - kPad - 96.f, slX = kPad + 44.f, slW = spinX - 8.f - slX;
        m_size->setBounds({ spinX, sizeY, 96.f, 26.f });
        m_sizeSlider->setBounds({ slX, sizeY + 2.f, slW, 24.f });   // slider at the top of the row; ticks sit beneath
        m_search->handleMouseMove(mx, my); m_list->handleMouseMove(mx, my); m_size->handleMouseMove(mx, my); m_sizeSlider->handleMouseMove(mx, my);
        if (pressed)  { m_search->handleMousePress(mx, my);   m_list->handleMousePress(mx, my);   m_size->handleMousePress(mx, my);   m_sizeSlider->handleMousePress(mx, my); }
        if (released) { m_search->handleMouseRelease(mx, my); m_list->handleMouseRelease(mx, my); m_size->handleMouseRelease(mx, my); m_sizeSlider->handleMouseRelease(mx, my); }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_list->handleScroll(mx, my, wheel);

        // Footer buttons (bottom of the dialog): Select (accent) and Cancel.
        if (pressed && my >= btnBarY() && my < btnBarY() + kDlgBtnH()) {
            if (mx >= selBtnX()    && mx < selBtnX()    + kDlgBtnW) { _accept(); return false; }
            if (mx >= cancelBtnX() && mx < cancelBtnX() + kDlgBtnW) { _cancel(); return false; }
        }

        _render(buf, mx, my, prevY, prevH);
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
    // Track-x for a size, matching JSlider's value->position mapping (value * width, measured from x).
    static float _sizeToX(int px, float slX, float slW) { return slX + _sizeToNorm(px) * slW; }
    // Preview box height = the displayed font's line height + padding, clamped so it never crowds the family
    // list out (keeps a minimum list height). Starts at 72px; grows once the font's line height exceeds it.
    float _previewH() const {
        const float fh = (m_prevValid ? m_prevAtlas.lineHeight : JTextHelper::lineHeight());
        const float listTop    = hdrH() + kRowH + 2.f * kPad;
        const float sizeRowTop = kH - kFooterH - kSizeRowH - kPad;
        const float maxPrev = std::max(72.f, sizeRowTop - 8.f - listTop - 80.f);   // keep >=80px for the list
        return std::clamp(fh + 20.f, 72.f, maxPrev);
    }
    std::string _pathForFamily(const std::string& fam) const {
        if (fam == "Default") return {};                       // "" = the framework's built-in face
        for (const auto& sf : m_systemFonts) if (sf.name == fam) return sf.path;
        return {};
    }
    void _accept() {
        if (m_onAcceptPath) { m_onAcceptPath(_pathForFamily(_selectedFamily()), m_size->value()); return; }   // app-font: FILE PATH + size
        const std::string fam = _selectedFamily();
        const std::string famSpec = (fam == "Default") ? std::string() : fam;   // Default = ""
        std::string spec = famSpec + "|" + std::to_string(m_size->value()) + "|" + (m_bold ? "1" : "0") + "|" + (m_ital ? "1" : "0");
        if (m_onAccept) m_onAccept(spec);
    }
    void _cancel() {}   // nothing to revert: browsing never changed the app font

    // Bake the highlighted family at the chosen size into a private GPU atlas used ONLY for the preview
    // line. "Default" (empty path) falls back to the base atlas (m_prevId 0). Rebuilds only on change,
    // and recycles the previous atlas so browsing doesn't leak GPU atlases.
    void _refreshPreview(JGpuHal& hal) {
        const std::string fam = _selectedFamily();
        const int bakePx = std::clamp(m_size ? m_size->value() : 12, 6, 96);   // full size-spin range
        if (fam == m_prevFamily && bakePx == m_prevPx) return;                 // unchanged
        m_prevFamily = fam; m_prevPx = bakePx;
        if (m_prevId) { hal.freeFontAtlas(m_prevId); m_prevId = 0; }
        m_prevValid = false;
        const std::string path = _pathForFamily(fam);
        if (path.empty()) return;                                             // Default → base atlas (id 0)
        if (!m_prevEngine.loadFromFile(path)) return;
        // Size the atlas bitmap to the bake px — the default 512x256 only fits ~14px text, so a larger
        // face overflows it and wraps into garbage. Same heuristic the framework's sized-glyph cache uses.
        uint32_t dim = static_cast<uint32_t>(std::ceil(bakePx * 13.0f));
        dim = std::clamp(dim, 512u, 2048u);
        m_prevAtlas = m_prevEngine.buildAtlas(static_cast<float>(bakePx), dim, dim);
        if (!m_prevAtlas.valid) return;
        m_prevId = hal.createFontAtlas(m_prevAtlas.bitmap.data(), m_prevAtlas.width, m_prevAtlas.height);
        m_prevValid = (m_prevId != 0);
    }

    void _render(JPrimitiveBuffer& buf, float mx, float my, float prevY, float prevH) {
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH), lh = JTextHelper::lineHeight();
        const float sizeY = kH - kFooterH - kSizeRowH - kPad;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);

        // Title bar — the SAME styled bar every other window uses (JStyle), with the canonical close button.
        const JRect closeR = JCloseButton::rectFor({ 0.f, 0.f, W, hdrH() });
        JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Select Font", JStyle::current().cornerRadius, 0, 12.f, closeR.width + 14.f);
        JCloseButton::draw(buf, closeR, JCloseButton::hit(closeR, mx, my));

        m_search->populateRenderPrimitives(buf);
        m_list->populateRenderPrimitives(buf);

        // Preview box + the sample rendered in the chosen face (its own atlas), clipped to the box.
        buf.pushRectangle(kPad, prevY, W - 2.f * kPad, prevH, Colors::PreviewBg, 5.f, 1.f, Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t sub[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, sub);
            const char* sample = "The quick brown fox jumps over the lazy dog.";
            const float availW = W - 2.f * kPad - 16.f;
            buf.pushClip(kPad, prevY, W - 2.f * kPad, prevH);
            if (m_prevValid) {
                const float plh = m_prevAtlas.lineHeight;
                JTextHelper::pushTextWith(buf, kPad + 8.f, prevY + (prevH - plh) * 0.5f, sample, sub, m_prevAtlas, m_prevId, availW);
            } else {   // "Default" / bake failed → the base app face
                JTextHelper::pushText(buf, kPad + 8.f, prevY + (prevH - lh) * 0.5f, sample, sub, availW);
            }
            buf.popClip();
            JTextHelper::pushText(buf, kPad, sizeY + (24.f - lh) * 0.5f, "Size", tc);
        }
        m_sizeSlider->populateRenderPrimitives(buf);
        m_size->populateRenderPrimitives(buf);

        // Common-size ticks + a labelled subset beneath the slider track, as reference marks.
        if (JTextHelper::hasAtlas()) {
            const float spinX = W - kPad - 96.f, slX = kPad + 44.f, slW = spinX - 8.f - slX;
            const float tickY = sizeY + 28.f;
            uint8_t tk[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tk);
            for (int s : kTickSizes) buf.pushRectangle(_sizeToX(s, slX, slW), tickY, 1.f, 5.f, tk, 0.f);
            for (int s : kLabelSizes) {
                const std::string t = std::to_string(s);
                JTextHelper::pushTextScaled(buf, _sizeToX(s, slX, slW) - JTextHelper::measureWidthScaled(t, 0.72f) * 0.5f,
                                            tickY + 6.f, t, tk, 0.72f);
            }
        }

        // Footer button bar (bottom of the dialog) — Cancel + Select, mirroring JNativeDialogWindow's layout.
        const float by = btnBarY(), okX = selBtnX(), caX = cancelBtnX();
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

    std::function<void(std::string)> m_onAccept;
    // App-font mode (empty unless setAppFontMode() was called): Select returns the chosen FILE PATH + size px.
    std::function<void(std::string,int)> m_onAcceptPath;
    // Preview line's private font: the highlighted family baked into its own GPU atlas so only the sample
    // renders in the chosen face (the app font is untouched until Select). m_prevId 0 => use the base atlas.
    JFontEngine m_prevEngine;
    JFontAtlas  m_prevAtlas;
    uint32_t    m_prevId{ 0 };
    bool        m_prevValid{ false };
    std::string m_prevFamily{ "\x01" };   // sentinel: forces the first bake
    int         m_prevPx{ 0 };
    std::vector<JSystemFont>         m_systemFonts;               // real installed fonts (name -> path)
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{ 0 };
    JSceneGraph  m_graph;
    // Owned, and declared AFTER m_graph so they're destroyed BEFORE it — each JWidget then deregisters from
    // JWidget::s_activeWidgets while its scene graph is still alive, so nothing dangles for the AI snapshot.
    std::unique_ptr<JLineEdit> m_search;
    std::unique_ptr<JListView> m_list;
    std::unique_ptr<JSlider>   m_sizeSlider;
    std::unique_ptr<JSpinBox>  m_size;
    std::vector<std::string>   m_allFamilies;
    bool m_syncing{ false };   // re-entrancy guard for the slider <-> spin two-way sync
    bool m_bold{ false }, m_ital{ false };
    bool m_done{ false }, m_drag{ false };
    float m_ax{ 0 }, m_ay{ 0 };
};

} // inline namespace jf
