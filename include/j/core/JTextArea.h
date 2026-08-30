#pragma once

// JTextArea — a multi-line text editor: soft word-wrap, a vertical scrollbar, an optional syntax
// highlighter, and rendering. The EDITING (buffer, caret, selection, word/char nav, clipboard, the key
// map) lives in the shared JTextEditCore (multi-line mode), the SAME model behind JLineEdit and the tree
// rename — so every field behaves identically and improves together. JTextArea owns only what needs the
// line layout: the wrap, the scroll, and the vertical/row-local motion (Up/Down, wrap-aware Home/End).

#include "JControl.h"
#include "JTextHelper.h"
#include "JTextEditCore.h"
#include "KeyEvent.h"

inline namespace jf {

// ============================================================================
// JTextArea
// ============================================================================

class JTextArea : public JControl {
public:
    jf::JSignal<std::string> onTextChanged;

    JTextArea(JSceneGraph& graph, const std::string& placeholder = "",
             float w = 340.0f, float h = 120.0f)
        : JControl(graph, "JTextArea"), m_placeholder(placeholder)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        l.minWidth = 100.0f;
        l.minHeight = 40.0f;
        m_core.setMultiline(true);   // Return inserts newlines, paste keeps them, Tab is insertable
    }

    void setText(const std::string& t) {
        if (m_core.text() != t) {
            m_core.setText(t);
            m_ensureCaret = true;
            m_layoutDirty = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_core.text());   // clamped value (the core applies maxLength)
        }
    }
    const std::string& text()        const { return m_core.text(); }
    const std::string& placeholder() const { return m_placeholder; }

    // Hard cap on the character count (0 = unlimited). Typing / Enter / paste that would exceed it are
    // rejected (a paste is truncated to fit); setText clamps too.
    void   setMaxLength(size_t n) {
        m_core.setMaxLength(n);
        m_layoutDirty = true; m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    size_t maxLength() const { return m_core.maxLength(); }

    std::string selectedText() const { return m_core.selectedText(); }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        // Scrollbar takes precedence over caret placement: grab the thumb, or page the view to a track click.
        if (m_hasScrollBar && mx >= m_sbX && mx <= m_sbX + m_sbW) {
            if (my >= m_sbThumbY && my <= m_sbThumbY + m_sbThumbH) { m_sbDragging = true; m_sbGrabDY = my - m_sbThumbY; }
            else _scrollThumbTo(my - m_sbThumbH * 0.5f);
            return;
        }
        m_ensureCaret = true;                  // a click positions the caret → keep it in view
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        float innerX = b.x + 8.0f;
        float innerY = b.y + 8.0f;
        float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;

        float relY = my - innerY + m_scrollOffset;
        auto lines = getLines();
        size_t clickLine = static_cast<size_t>(std::max(0.0f, relY / lh));
        if (clickLine >= lines.size()) clickLine = lines.empty() ? 0 : lines.size() - 1;

        float relX = mx - innerX;
        size_t clickCol = 0;
        if (JTextHelper::hasAtlas() && clickLine < lines.size()) {
            const std::string& ln = lines[clickLine];
            float cx = 0;
            for (size_t i = 0; i < ln.size(); ++i) {
                float cw = JTextHelper::measureWidth(ln.substr(i, 1));
                if (cx + cw * 0.5f > relX) { clickCol = i; goto done_click; }
                cx += cw;
            }
            clickCol = ln.size();
        } else {
            if (clickLine < lines.size()) {
                clickCol = static_cast<size_t>(std::max(0.0f, relX / 6.0f));
                if (clickCol > lines[clickLine].size()) clickCol = lines[clickLine].size();
            }
        }
        done_click:
        m_core.setCaret(getPosFromLineCol(clickLine, clickCol), /*extend=*/false);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_sbDragging) _scrollThumbTo(my - m_sbGrabDY);
    }
    void handleMouseRelease(float mx, float my) override { m_sbDragging = false; JControl::handleMouseRelease(mx, my); }

    // Position the view so the scroll thumb's top lands at `thumbTopY` (screen). Used by thumb-drag + track-click.
    void _scrollThumbTo(float thumbTopY) {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;
        const float innerH = b.height - 16.0f;
        const float maxScroll = std::max(0.0f, static_cast<float>(getLines().size()) * lh - innerH);
        const float range = m_sbTrackH - m_sbThumbH;
        const float t = range > 0.0f ? std::clamp((thumbTopY - m_sbY) / range, 0.0f, 1.0f) : 0.0f;
        m_scrollOffset = t * maxScroll;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // ---- Visual rows (soft word-wrap) --------------------------------------------------------------
    // Every geometry op (render / cursor / click / scroll) works on VISUAL rows so long lines wrap inside
    // the widget instead of spilling out. A row is a byte range [start, start+len) of the text with NO '\n';
    // rows come from splitting logical lines (on '\n') AND wrapping any line wider than the text area.
    struct VRow { size_t start; size_t len; };

    float _wrapWidth() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return std::max(40.0f, b.width - 16.0f - 10.0f);   // inner width minus the scrollbar gutter
    }
    // Recompute the wrapped rows + syntax colours — ONLY when the text or width changed (m_layoutDirty).
    void _ensureLayout() const {
        const std::string& text = m_core.text();
        const float w = _wrapWidth();
        if (!m_layoutDirty && w == m_layoutW) return;
        m_layoutW = w; m_layoutDirty = false;

        const bool atlas = JTextHelper::hasAtlas();
        float adv[128];
        if (atlas) { const auto& atl = JTextHelper::atlas();
            for (int i = 0; i < 128; ++i) { auto it = atl.glyphs.find(static_cast<uint32_t>(i));
                adv[i] = (it != atl.glyphs.end()) ? it->second.advanceX : atl.ascent * 0.35f; } }
        auto cw = [&](unsigned char c) -> float { return atlas ? (c < 128 ? adv[c] : 8.0f) : 6.0f; };

        m_rows.clear();
        size_t lineStart = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i != text.size() && text[i] != '\n') continue;
            if (lineStart >= i) { m_rows.push_back({lineStart, 0}); }
            else {
                size_t rowStart = lineStart;
                while (rowStart < i) {                                     // wrap the logical line [lineStart, i)
                    float acc = 0.f; size_t j = rowStart, lastSpace = std::string::npos, brk = i;
                    while (j < i) {
                        const float a = cw(static_cast<unsigned char>(text[j]));
                        if (acc + a > w && j > rowStart) {
                            brk = (lastSpace != std::string::npos && lastSpace + 1 > rowStart) ? lastSpace + 1 : j;
                            break;
                        }
                        if (text[j] == ' ' || text[j] == '\t') lastSpace = j;
                        acc += a; ++j; brk = j;
                    }
                    m_rows.push_back({rowStart, brk - rowStart});
                    rowStart = brk;
                }
            }
            lineStart = i + 1;
            if (i == text.size()) break;
        }
        if (m_rows.empty()) m_rows.push_back({0, 0});

        m_hcols.clear();
        if (m_highlighter && !text.empty()) m_highlighter(text, m_hcols);   // syntax colours: once per change
    }
    const std::vector<VRow>& visualRows() const { _ensureLayout(); return m_rows; }
    std::string _rowText(const VRow& r) const { return m_core.text().substr(r.start, r.len); }

    std::vector<std::string> getLines() const {
        std::vector<std::string> lines;
        for (const VRow& r : visualRows()) lines.push_back(_rowText(r));
        return lines;
    }

    void getCursorLineCol(size_t& outLine, size_t& outCol) const {
        const auto& rows = visualRows();
        const size_t cur = m_core.caret();
        outLine = 0; outCol = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            const size_t rowEnd = rows[r].start + rows[r].len;
            const bool last = (r + 1 == rows.size());
            if (cur <= rowEnd || last) {                                   // caret sits on this visual row
                if (!last && cur == rowEnd + 1) continue;                  // exactly on the '\n' → next row
                outLine = r;
                outCol  = cur >= rows[r].start ? cur - rows[r].start : 0;
                if (outCol > rows[r].len) outCol = rows[r].len;
                return;
            }
        }
    }

    size_t getPosFromLineCol(size_t line, size_t col) const {
        const auto& rows = visualRows();
        if (rows.empty()) return 0;
        if (line >= rows.size()) line = rows.size() - 1;
        if (col > rows[line].len) col = rows[line].len;
        return rows[line].start + col;
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        m_ensureCaret = true;                  // typing / navigating moves the caret → keep it in view

        // Vertical / row-local motion needs the wrap layout, so it stays here; the caret index it computes
        // goes back through the core (which owns the anchor/selection semantics).
        if (ke.key == K::Up || ke.key == K::Down || ke.key == K::Home || ke.key == K::End) {
            size_t line = 0, col = 0; getCursorLineCol(line, col);
            const auto lines = getLines();
            size_t newPos = m_core.caret();
            if      (ke.key == K::Up)   { if (line > 0) newPos = getPosFromLineCol(line - 1, col); }
            else if (ke.key == K::Down) { if (line + 1 < lines.size()) newPos = getPosFromLineCol(line + 1, col); }
            else if (ke.key == K::Home) { newPos = getPosFromLineCol(line, 0); }
            else /* End */              { if (line < lines.size()) newPos = getPosFromLineCol(line, lines[line].size()); }
            m_core.setCaret(newPos, /*extend=*/ke.shift);
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            return true;
        }

        // Everything else — text, char/word motion + selection, backspace/delete, clipboard, newline — is the
        // shared core. A text change reflows the cached layout.
        const auto res = m_core.handleKey(ke);
        if (res.changed)       { m_layoutDirty = true; m_graph.invalidateNode(m_nodeId, DirtySelf); onTextChanged.emit(m_core.text()); }
        else if (res.consumed) { m_graph.invalidateNode(m_nodeId, DirtySelf); }
        return res.consumed;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const std::string& text = m_core.text();
        bool focused = isFocused();

        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1,
                          JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused),
                          focused ? Colors::Accent : Colors::Border);

        float innerX = b.x + 8.0f;
        float innerY = b.y + 8.0f;
        float innerW = b.width - 16.0f;
        float innerH = b.height - 16.0f;

        float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;

        size_t cursorLine = 0, cursorCol = 0;
        getCursorLineCol(cursorLine, cursorCol);

        const auto& rows = visualRows();

        if (m_ensureCaret) {
            float cursorYRel = cursorLine * lh;
            if (cursorYRel < m_scrollOffset)                 m_scrollOffset = cursorYRel;
            else if (cursorYRel + lh > m_scrollOffset + innerH) m_scrollOffset = cursorYRel + lh - innerH;
            m_ensureCaret = false;
        }
        const float maxScroll = std::max(0.0f, static_cast<float>(rows.size()) * lh - innerH);
        m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxScroll);

        buf.pushClip(innerX, innerY, innerW, innerH);

        auto rowX = [&](const std::string& t, size_t nchars) -> float {
            return innerX + (JTextHelper::hasAtlas() ? JTextHelper::measureWidth(t.substr(0, nchars))
                                                     : static_cast<float>(nchars) * 6.0f);
        };

        // Selection highlight — per visual row, the intersection of the row's byte range with [selLo, selHi).
        // Focused only — see JLineEdit: a selection is where the next keystroke lands, and one drawn on a
        // field that cannot receive keys is a claim it has no business making. The caret is already gated
        // this way; the band was not, so every field you had ever selected text in stayed lit.
        if (focused && m_core.hasSelection()) {
            const size_t selLo = m_core.selectionStart(), selHi = m_core.selectionEnd();
            const uint8_t selColor[4] = {Colors::SelectionFill[0], Colors::SelectionFill[1], Colors::SelectionFill[2], 100};
            for (size_t r = 0; r < rows.size(); ++r) {
                const float lineY = innerY + r * lh - m_scrollOffset;
                if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                const size_t rs = rows[r].start, re = rs + rows[r].len;
                const size_t a = std::max(selLo, rs), bb = std::min(selHi, re);
                if (bb <= a) continue;
                const std::string rowT = _rowText(rows[r]);
                const float sx = rowX(rowT, a - rs), ex = rowX(rowT, bb - rs);
                if (ex > sx) buf.pushRectangle(sx, lineY, ex - sx, lh, selColor);
            }
        }

        if (JTextHelper::hasAtlas()) {
            const std::vector<uint8_t>& cols = m_hcols;
            const uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 220};
            if (text.empty() && !m_placeholder.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 160};
                JTextHelper::pushText(buf, innerX, innerY, m_placeholder, pc, innerW);
            } else {
                for (size_t i = 0; i < rows.size(); ++i) {
                    const float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                    const std::string ln = _rowText(rows[i]);
                    if (cols.empty()) { JTextHelper::pushText(buf, innerX, lineY, ln, tc, innerW); continue; }
                    const size_t off = rows[i].start;                    // ABSOLUTE byte offset of this row's start
                    auto colAt = [&](size_t c, uint8_t out[4]) {
                        const size_t ci = (off + c) * 4;
                        if (ci + 3 < cols.size()) { out[0]=cols[ci]; out[1]=cols[ci+1]; out[2]=cols[ci+2]; out[3]=cols[ci+3]; }
                        else { out[0]=tc[0]; out[1]=tc[1]; out[2]=tc[2]; out[3]=tc[3]; }
                    };
                    float x = innerX;
                    for (size_t j = 0; j < ln.size(); ) {
                        uint8_t rc[4]; colAt(j, rc);
                        size_t k = j + 1;
                        for (; k < ln.size(); ++k) { uint8_t kc[4]; colAt(k, kc); if (kc[0]!=rc[0]||kc[1]!=rc[1]||kc[2]!=rc[2]||kc[3]!=rc[3]) break; }
                        const std::string run = ln.substr(j, k - j);
                        JTextHelper::pushText(buf, x, lineY, run, rc, innerW);
                        x += JTextHelper::measureWidth(run);
                        j = k;
                    }
                }
            }
        } else {
            if (text.empty()) {
                uint8_t pc[4] = {Colors::FieldPlaceholder[0], Colors::FieldPlaceholder[1], Colors::FieldPlaceholder[2], 120};
                buf.pushRectangle(innerX, innerY + (lh - 7.0f) * 0.5f, innerW * 0.55f, 7.0f, pc, 2.0f);
            } else {
                uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 200};
                for (size_t i = 0; i < rows.size(); ++i) {
                    float lineY = innerY + i * lh - m_scrollOffset;
                    if (lineY + lh < innerY || lineY > innerY + innerH) continue;
                    float lw = std::min(innerW, 20.0f + static_cast<float>(rows[i].len * 6));
                    buf.pushRectangle(innerX, lineY + (lh - 7.0f) * 0.5f, lw, 7.0f, tc, 2.0f);
                }
            }
        }

        // Caret at the cursor's visual row/column.
        if (focused) {
            float cx = innerX;
            if (!text.empty() && cursorLine < rows.size())
                cx = rowX(_rowText(rows[cursorLine]), std::min(cursorCol, rows[cursorLine].len));
            float cy = innerY + cursorLine * lh - m_scrollOffset;
            if (cy + lh >= innerY && cy <= innerY + innerH)
                buf.pushRectangle(cx, cy + 2.0f, 1.5f, lh - 4.0f, Colors::Accent);
        }

        buf.popClip();

        // Vertical scrollbar — shown only when the content overflows.
        const float contentH = static_cast<float>(rows.size()) * lh;
        m_hasScrollBar = contentH > innerH + 1.0f;
        if (m_hasScrollBar) {
            const float sbW = 8.0f;
            m_sbX = b.x + b.width - sbW - 3.0f; m_sbY = b.y + 4.0f; m_sbW = sbW; m_sbTrackH = b.height - 8.0f;
            buf.pushRectangle(m_sbX, m_sbY, sbW, m_sbTrackH, Colors::Surface0, sbW * 0.5f);
            const float maxScroll2 = contentH - innerH;
            m_sbThumbH = std::max(24.0f, m_sbTrackH * (innerH / contentH));
            const float frac = maxScroll2 > 0.0f ? (m_scrollOffset / maxScroll2) : 0.0f;
            m_sbThumbY = m_sbY + frac * (m_sbTrackH - m_sbThumbH);
            buf.pushRectangle(m_sbX, m_sbThumbY, sbW, m_sbThumbH, Colors::Surface3, sbW * 0.5f);
        }
    }

    // Mouse wheel scrolls the view (independently of the caret).
    bool handleScroll(float mx, float my, float wheel) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        if (mx < b.x || mx > b.x + b.width || my < b.y || my > b.y + b.height) return false;
        const float lh = JTextHelper::hasAtlas() ? JTextHelper::lineHeight() : 12.0f;
        const float innerH = b.height - 16.0f;
        const float maxScroll = std::max(0.0f, static_cast<float>(getLines().size()) * lh - innerH);
        if (maxScroll <= 0.0f) return false;
        m_scrollOffset = std::clamp(m_scrollOffset - wheel * lh * 3.0f, 0.0f, maxScroll);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        return true;
    }

    // Optional syntax highlighter: fills `out` with 4 bytes (RGBA) per character; the render draws each line
    // as runs of equal colour. Null (default) → the whole text draws in one colour. Used by the Lua editor.
    void setHighlighter(std::function<void(const std::string&, std::vector<uint8_t>&)> h) { m_highlighter = std::move(h); m_layoutDirty = true; m_graph.invalidateNode(m_nodeId, DirtySelf); }

private:
    std::function<void(const std::string&, std::vector<uint8_t>&)> m_highlighter;   // null = plain single-colour text

    JTextEditCore m_core;              // the shared text-editing model (multi-line); owns text/caret/selection
    std::string m_placeholder;
    float       m_scrollOffset{0.0f};
    bool        m_ensureCaret{true};   // scroll to the caret next render (set on caret-moving actions)
    // Vertical scrollbar geometry, recomputed each render; used for wheel + thumb-drag hit-testing.
    bool        m_hasScrollBar{false};
    float       m_sbX{0}, m_sbY{0}, m_sbW{0}, m_sbTrackH{0}, m_sbThumbY{0}, m_sbThumbH{0};
    bool        m_sbDragging{false};
    float       m_sbGrabDY{0};
    // Cached line layout (wrapped rows + syntax colours) — recomputed only on a text or width change.
    mutable std::vector<VRow>   m_rows;
    mutable std::vector<uint8_t> m_hcols;         // per-char RGBA syntax colours (empty = no highlighter)
    mutable bool                m_layoutDirty{true};
    mutable float               m_layoutW{-1.0f};
};

} // inline namespace jf
