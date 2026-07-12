#pragma once

// ============================================================================
// JFileDialogWindow — the framework's in-app file/folder picker, rendered as its
// own OS-level popup window (same model as JNativeDialogWindow). No zenity, no
// GetOpenFileName, no shelling out — the directory tree is walked with
// std::filesystem and drawn by the toolkit, so the picker looks and behaves the
// same on every platform and leaks no platform types past this file.
//
// Driven by a file-kind JDialogRequest (OpenFile / SaveFile / OpenFolder):
//   title       — window caption
//   extensions  — filter list ("json", "gui"); empty = show every file
//   onInput     — accept callback, receives the chosen absolute path
//   onCancel    — cancel / close / Escape
// ============================================================================

#include <j/core/Dialog.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JFileDialogWindow {
public:
    static constexpr uint32_t kW           = 560;   // window width (this window's own layout data)
    static constexpr int      kVisibleRows = 12;    // file rows shown before scrolling

#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    JFileDialogWindow(JDialogRequest req, JGpuHal& hal,
                      int screenX, int screenY,
                      NativeWinHandleType parentWindow = {})
        : m_req(std::move(req))
        , m_winH(calcHeight())
        , m_window(std::make_unique<PlatformWinType>(
              m_req.title.c_str(), kW, m_winH, screenX, screenY,
              JPlatformWindowStyle::Borderless, parentWindow))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, m_winH))
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path start = s_lastDir.empty() ? fs::current_path(ec) : fs::path(s_lastDir);
        if (ec || !fs::exists(start, ec)) start = fs::path("/");
        _navigate(start);
        if (m_req.kind == JDialogRequest::JKind::SaveFile)
            m_filename = _defaultSaveName();
    }

    JFileDialogWindow(const JFileDialogWindow&)            = delete;
    JFileDialogWindow& operator=(const JFileDialogWindow&) = delete;
    JFileDialogWindow(JFileDialogWindow&&)                 = default;
    JFileDialogWindow& operator=(JFileDialogWindow&&)      = default;

    // Overall height, computed from theme dims so layout and window agree.
    static uint32_t calcHeight() {
        const auto& th = JStyle::current();
        const float rowH = th.controlHeight, btnH = th.buttonHeight, titleH = th.titleBarHeight;
        const bool  wantsName = true;  // name field kept for all modes for a stable layout
        float h = titleH
                + kPad + rowH                         // path bar
                + kPad + kVisibleRows * rowH          // file list
                + (wantsName ? kPad + rowH : 0.f)     // filename field
                + kPad + btnH                         // buttons
                + kPad;                               // bottom margin
        return static_cast<uint32_t>(h);
    }

    // Returns false when dismissed — caller should destroySurface() then erase.
    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        if (m_done) return false;

        m_window->pollNativeEvents();

        m_mx      = m_window->mouseX();
        m_my      = m_window->mouseY();
        m_pressed = m_window->consumePress();
        m_held    = m_window->isLeftButtonDown();
        m_wheel   = m_window->consumeWheel();
        m_keys    = m_window->consumeAllKeys();

        _handleKeys();
        if (m_done) return false;
        _handleDrag();

        buf.clear();
        _render(buf);
        if (m_done) return false;

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }
    bool isModal() const { return m_req.options.modal; }

private:
    struct Entry { std::string name; bool isDir; };

    static constexpr float kPad     = 12.f;   // uniform layout gutter for this window
    static constexpr float kRowIcon = 8.f;    // dir/file marker square edge
    static constexpr int   kDblMs   = 400;    // double-click window

    static std::string s_lastDir;             // remembered across opens

    // ---- Directory model ---------------------------------------------------
    void _navigate(const std::filesystem::path& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(dir, ec);
        if (ec) canon = dir;
        m_cwd = canon;
        s_lastDir = m_cwd.string();
        m_entries.clear();
        m_selected = -1;
        m_scroll = 0;

        std::vector<Entry> dirs, files;
        for (fs::directory_iterator it(m_cwd, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            std::string name = p.filename().string();
            if (name.empty() || name[0] == '.') continue;   // hide dotfiles
            std::error_code dec;
            if (it->is_directory(dec)) dirs.push_back({name, true});
            else if (_passesFilter(name)) files.push_back({name, false});
        }
        auto byName = [](const Entry& a, const Entry& b){ return a.name < b.name; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        m_entries = std::move(dirs);
        m_entries.insert(m_entries.end(), files.begin(), files.end());
    }

    bool _passesFilter(const std::string& name) const {
        if (m_req.extensions.empty()) return true;
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        for (const auto& e : m_req.extensions) {
            std::string want = e;
            std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c){ return std::tolower(c); });
            if (ext == want) return true;
        }
        return false;
    }

    std::string _defaultSaveName() const {
        if (!m_req.extensions.empty()) return std::string("untitled.") + m_req.extensions.front();
        return "untitled";
    }

    std::string _filterHint() const {
        if (m_req.extensions.empty()) return "All files";
        std::string s;
        for (const auto& e : m_req.extensions) { if (!s.empty()) s += " "; s += "*." + e; }
        return s;
    }

    // ---- Activation / accept ----------------------------------------------
    void _goUp() {
        if (m_cwd.has_parent_path() && m_cwd.parent_path() != m_cwd) _navigate(m_cwd.parent_path());
    }

    void _activate(int idx) {
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        const Entry& e = m_entries[idx];
        if (e.isDir) {
            if (m_req.kind == JDialogRequest::JKind::OpenFolder) { m_selected = idx; }
            _navigate(m_cwd / e.name);
        } else {
            m_filename = e.name;
            _accept();
        }
    }

    void _accept() {
        namespace fs = std::filesystem;
        std::string path;
        if (m_req.kind == JDialogRequest::JKind::OpenFolder) {
            fs::path chosen = (m_selected >= 0 && m_selected < (int)m_entries.size() && m_entries[m_selected].isDir)
                            ? (m_cwd / m_entries[m_selected].name) : m_cwd;
            path = chosen.string();
        } else {
            if (m_filename.empty()) return;   // nothing to open/save
            std::string fname = m_filename;
            if (m_req.kind == JDialogRequest::JKind::SaveFile && !m_req.extensions.empty()
                && fname.find('.') == std::string::npos) {
                fname += "." + m_req.extensions.front();
            }
            path = (m_cwd / fname).string();
        }
        if (m_req.onInput) m_req.onInput(path);
        m_done = true;
    }

    void _cancel() {
        if (m_req.onCancel) m_req.onCancel();
        m_done = true;
    }

    // ---- Input -------------------------------------------------------------
    void _handleKeys() {
        const JDialogKeyBindings& kb = JDialogManager::keyBindings();
        const bool typing = (m_req.kind != JDialogRequest::JKind::OpenFolder);
        for (const auto& ke : m_keys) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if      (ke.key == kb.cancel)        { _cancel(); return; }
            else if (ke.key == K::Up)            { if (m_selected > 0) { m_selected--; _syncName(); _ensureVisible(); } }
            else if (ke.key == K::Down)          { if (m_selected + 1 < (int)m_entries.size()) { m_selected++; _syncName(); _ensureVisible(); } }
            else if (ke.key == K::PageUp)        { m_selected = std::max(0, m_selected - kVisibleRows); _syncName(); _ensureVisible(); }
            else if (ke.key == K::PageDown)      { m_selected = std::min((int)m_entries.size() - 1, m_selected + kVisibleRows); _syncName(); _ensureVisible(); }
            else if (ke.key == K::Home)          { m_selected = m_entries.empty() ? -1 : 0; _syncName(); _ensureVisible(); }
            else if (ke.key == K::End)           { m_selected = (int)m_entries.size() - 1; _syncName(); _ensureVisible(); }
            else if (ke.key == kb.accept)        { if (m_selected >= 0) _activate(m_selected); else _accept(); if (m_done) return; }
            else if (ke.key == K::Backspace)     { if (typing && !m_filename.empty()) m_filename.pop_back(); else if (!typing) _goUp(); }
            else if (typing && ke.utf8[0] >= 0x20) { m_filename += ke.utf8; }
        }
    }

    void _syncName() {
        if (m_selected >= 0 && m_selected < (int)m_entries.size() && !m_entries[m_selected].isDir)
            m_filename = m_entries[m_selected].name;
    }

    void _ensureVisible() {
        if (m_selected < 0) return;
        if (m_selected < m_scroll) m_scroll = m_selected;
        else if (m_selected >= m_scroll + kVisibleRows) m_scroll = m_selected - kVisibleRows + 1;
    }

    void _handleDrag() {
        const float titleH = JStyle::current().titleBarHeight;
        const float titleDragW = (float)kW - JStyle::current().titleBarHeight;   // leave the close square
        bool inTitle = (m_mx >= 0.f && m_mx < titleDragW && m_my >= 0.f && m_my < titleH);
        if (m_held && inTitle && !m_dragging) { m_dragging = true; m_dragAnchorX = m_mx; m_dragAnchorY = m_my; }
        if (m_dragging) {
            auto [gx, gy] = m_window->globalCursorPos();
            m_window->setPosition(gx - (int)m_dragAnchorX, gy - (int)m_dragAnchorY);
        }
        if (!m_held) m_dragging = false;
    }

    // ---- Render ------------------------------------------------------------
    void _render(JPrimitiveBuffer& buf) {
        const auto& th = JStyle::current();
        const float W = (float)kW, H = (float)m_winH;
        const float R = th.cornerRadius, titleH = th.titleBarHeight;
        const float rowH = th.controlHeight, btnH = th.buttonHeight;
        const float lh = JTextHelper::lineHeight();

        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, R, 1.f, Colors::Border);

        // Title bar + close (composed, one code path shared with every window).
        JTitleBar::draw(buf, 0.f, 0.f, W, titleH, m_req.title, R, /*align*/0, 14.f, titleH);
        const JRect cr = JCloseButton::rectFor({0.f, 0.f, W, titleH});
        bool hovClose = _hit(cr.x, cr.y, cr.width, cr.height);
        JCloseButton::draw(buf, cr, hovClose);
        if (m_pressed && hovClose) { _cancel(); return; }

        float y = titleH + kPad;

        // ---- Path bar: current dir + Up button -----------------------------
        const float upW = 34.f;
        const float pathW = W - 2 * kPad - upW - kPad;
        buf.pushRectangle(kPad, y, pathW, rowH, Colors::InputFieldBg, R);
        JTextHelper::pushText(buf, kPad + 8.f, y + (rowH - lh) * 0.5f, m_cwd.string(),
                              Colors::TextSecondary, pathW - 16.f);
        const float upX = kPad + pathW + kPad;
        bool hovUp = _hit(upX, y, upW, rowH);
        buf.pushRectangle(upX, y, upW, rowH, hovUp ? Colors::CancelBtnBorder : Colors::CancelBtnBg, R, 1.f, Colors::CancelBtnBorder);
        JTextHelper::pushText(buf, upX + (upW - JTextHelper::measureWidth("Up")) * 0.5f,
                              y + (rowH - lh) * 0.5f, "Up", Colors::ControlText);
        if (m_pressed && hovUp) { _goUp(); return; }
        y += rowH + kPad;

        // ---- File list -----------------------------------------------------
        const float listX = kPad, listW = W - 2 * kPad;
        const float listY = y, listH = kVisibleRows * rowH;
        buf.pushRectangle(listX, listY, listW, listH, Colors::InputFieldBg, R, 1.f, Colors::Border);

        // Wheel scroll (bounded).
        if (m_wheel != 0.f && _hit(listX, listY, listW, listH)) {
            m_scroll -= (int)m_wheel;
        }
        const int maxScroll = std::max(0, (int)m_entries.size() - kVisibleRows);
        m_scroll = std::clamp(m_scroll, 0, maxScroll);

        for (int row = 0; row < kVisibleRows; ++row) {
            int idx = m_scroll + row;
            if (idx >= (int)m_entries.size()) break;
            const Entry& e = m_entries[idx];
            float ry = listY + row * rowH;
            bool hov = _hit(listX, ry, listW, rowH);
            if (idx == m_selected)
                buf.pushRectangle(listX + 1.f, ry, listW - 2.f, rowH, Colors::SelectionFill, 0.f);
            else if (hov)
                buf.pushRectangle(listX + 1.f, ry, listW - 2.f, rowH, Colors::RowAltBg, 0.f);

            // dir/file marker square: dirs accented, files muted.
            float iconY = ry + (rowH - kRowIcon) * 0.5f;
            buf.pushRectangle(listX + 10.f, iconY, kRowIcon, kRowIcon,
                              e.isDir ? Colors::Accent : Colors::MutedText, 2.f);
            const uint8_t* tc = (idx == m_selected) ? Colors::TextPrimary
                              : e.isDir ? Colors::ControlText : Colors::TextPrimary;
            std::string label = e.isDir ? (e.name + "/") : e.name;
            JTextHelper::pushText(buf, listX + 26.f, ry + (rowH - lh) * 0.5f, label, tc, listW - 36.f);

            if (m_pressed && hov) {
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
                bool dbl = (idx == m_lastClickIdx) && (now - m_lastClickMs < kDblMs);
                m_lastClickIdx = idx; m_lastClickMs = now;
                m_selected = idx; _syncName();
                if (dbl) { _activate(idx); return; }
            }
        }

        // Scroll thumb (only when the list overflows).
        if (maxScroll > 0) {
            float trackH = listH;
            float thumbH = std::max(24.f, trackH * kVisibleRows / (float)m_entries.size());
            float thumbY = listY + (trackH - thumbH) * (float)m_scroll / (float)maxScroll;
            buf.pushRectangle(listX + listW - 6.f, thumbY, 4.f, thumbH, Colors::ScrollThumb, 2.f);
        }
        y = listY + listH + kPad;

        // ---- Filename field ------------------------------------------------
        const float nameLabelW = 44.f;
        JTextHelper::pushText(buf, kPad, y + (rowH - lh) * 0.5f, "Name:", Colors::TextSecondary);
        const float fX = kPad + nameLabelW, fW = W - 2 * kPad - nameLabelW;
        bool folderMode = (m_req.kind == JDialogRequest::JKind::OpenFolder);
        buf.pushRectangle(fX, y, fW, rowH, Colors::InputFieldBg, R, 1.f,
                          folderMode ? Colors::Border : Colors::Accent);
        float textY = y + (rowH - lh) * 0.5f;
        if (folderMode) {
            std::string shown = (m_selected >= 0 && m_selected < (int)m_entries.size())
                              ? m_entries[m_selected].name : std::string("<this folder>");
            JTextHelper::pushText(buf, fX + 8.f, textY, shown, Colors::TextSecondary, fW - 16.f);
        } else {
            JTextHelper::pushText(buf, fX + 8.f, textY, m_filename, Colors::TextPrimary, fW - 16.f);
            float cx = fX + 8.f + JTextHelper::measureWidth(m_filename) + 1.f;
            uint8_t cc[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 220};
            buf.pushRectangle(cx, textY + 1.f, 2.f, lh - 2.f, cc, 0.f);
        }
        y += rowH + kPad;

        // ---- Buttons + filter hint ----------------------------------------
        const float bW = 96.f, by = H - btnH - kPad;
        JTextHelper::pushText(buf, kPad, by + (btnH - lh) * 0.5f, _filterHint(), Colors::MutedText, W * 0.4f);

        const char* okLbl = (m_req.kind == JDialogRequest::JKind::SaveFile) ? "Save"
                          : (m_req.kind == JDialogRequest::JKind::OpenFolder) ? "Choose" : "Open";
        float okX = W - bW - kPad, cancelX = W - 2 * bW - kPad - 8.f;

        bool hovCancel = _hit(cancelX, by, bW, btnH);
        buf.pushRectangle(cancelX, by, bW, btnH, Colors::CancelBtnBg, R, 1.f, Colors::CancelBtnBorder);
        JTextHelper::pushText(buf, cancelX + (bW - JTextHelper::measureWidth("Cancel")) * 0.5f,
                              by + (btnH - lh) * 0.5f, "Cancel", Colors::CancelBtnText);
        if (m_pressed && hovCancel) { _cancel(); return; }

        bool hovOk = _hit(okX, by, bW, btnH);
        uint8_t okBg[4] = {Colors::PrimaryBtnBg[0], Colors::PrimaryBtnBg[1], Colors::PrimaryBtnBg[2],
                            static_cast<uint8_t>(hovOk ? 255 : 220)};
        buf.pushRectangle(okX, by, bW, btnH, okBg, R, 1.f, Colors::PrimaryBtnBorder);
        JTextHelper::pushText(buf, okX + (bW - JTextHelper::measureWidth(okLbl)) * 0.5f,
                              by + (btnH - lh) * 0.5f, okLbl, Colors::PrimaryBtnText);
        if (m_pressed && hovOk) { _accept(); return; }
    }

    bool _hit(float x, float y, float w, float h) const {
        return m_mx >= x && m_mx < x + w && m_my >= y && m_my < y + h;
    }

    JDialogRequest                   m_req;
    uint32_t                         m_winH;
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId                     m_surface{0};

    std::filesystem::path            m_cwd;
    std::vector<Entry>               m_entries;
    int                              m_selected{-1};
    int                              m_scroll{0};
    std::string                      m_filename;

    bool  m_done{false};
    bool  m_dragging{false};
    float m_dragAnchorX{0}, m_dragAnchorY{0};
    int   m_lastClickIdx{-1};
    int64_t m_lastClickMs{0};

    float m_mx{0}, m_my{0};
    bool  m_pressed{false}, m_held{false};
    float m_wheel{0};
    std::vector<JKeyEvent> m_keys;
};

inline std::string JFileDialogWindow::s_lastDir;

} // inline namespace jf
