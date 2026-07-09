#pragma once

// JTextArea.

#include "JControl.h"
#include "JTextHelper.h"
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
        l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
        l.minWidth = 100.0f;
        l.minHeight = 40.0f;
    }

    void setText(const std::string& t) {
        const std::string v = (m_maxLen && t.size() > m_maxLen) ? t.substr(0, m_maxLen) : t;
        if (m_text != v) {
            m_text = v;
            m_cursorPos = m_text.size();
            m_ensureCaret = true;              // scroll to the caret on the next render (cursor moved)
            m_layoutDirty = true;              // text changed → reflow the cached layout
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);        // clamped value (not the raw argument)
        }
    }
    const std::string& text()        const { return m_text; }
    const std::string& placeholder() const { return m_placeholder; }

    // Hard cap on the character count (0 = unlimited). Typing / Enter / paste that would exceed it are
    // rejected (a paste is truncated to fit); setText clamps too. Use it to bind an editor to a fixed-size
    // backing field so the user can't author more than the field holds.
    void   setMaxLength(size_t n) {
        m_maxLen = n;
        if (m_maxLen && m_text.size() > m_maxLen) {          // clamp any existing over-length text
            m_text.resize(m_maxLen);
            if (m_cursorPos > m_maxLen) m_cursorPos = m_maxLen;
            m_layoutDirty = true; m_graph.invalidateNode(m_nodeId, DirtySelf);
        }
    }
    size_t maxLength() const { return m_maxLen; }
    bool   _full() const { return m_maxLen && m_text.size() >= m_maxLen; }

    std::string selectedText() const {
        if (!m_selActive || m_selStart == m_selEnd) return {};
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        return m_text.substr(lo, hi - lo);
    }

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
        m_cursorPos = getPosFromLineCol(clickLine, clickCol);
        m_selActive = false;
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
    // the widget instead of spilling out. A row is a byte range [start, start+len) of m_text with NO '\n';
    // rows come from splitting logical lines (on '\n') AND wrapping any line wider than the text area at a
    // space boundary (falling back to mid-word for a single over-long token).
    struct VRow { size_t start; size_t len; };

    float _wrapWidth() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        return std::max(40.0f, b.width - 16.0f - 10.0f);   // inner width minus the scrollbar gutter
    }
    // Recompute the wrapped rows + syntax colours — ONLY when the text or width changed (marked by
    // m_layoutDirty). Idle frames and cursor navigation reuse the cache; the render just draws + culls it.
    // Wrapping uses a per-ASCII advance table (no per-char measure / substr), so a reflow is one cheap O(n)
    // pass, not O(n) expensive calls per frame.
    void _ensureLayout() const {
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
        for (size_t i = 0; i <= m_text.size(); ++i) {
            if (i != m_text.size() && m_text[i] != '\n') continue;
            if (lineStart >= i) { m_rows.push_back({lineStart, 0}); }
            else {
                size_t rowStart = lineStart;
                while (rowStart < i) {                                     // wrap the logical line [lineStart, i)
                    float acc = 0.f; size_t j = rowStart, lastSpace = std::string::npos, brk = i;
                    while (j < i) {
                        const float a = cw(static_cast<unsigned char>(m_text[j]));
                        if (acc + a > w && j > rowStart) {
                            brk = (lastSpace != std::string::npos && lastSpace + 1 > rowStart) ? lastSpace + 1 : j;
                            break;
                        }
                        if (m_text[j] == ' ' || m_text[j] == '\t') lastSpace = j;
                        acc += a; ++j; brk = j;
                    }
                    m_rows.push_back({rowStart, brk - rowStart});
                    rowStart = brk;
                }
            }
            lineStart = i + 1;
            if (i == m_text.size()) break;
        }
        if (m_rows.empty()) m_rows.push_back({0, 0});

        m_hcols.clear();
        if (m_highlighter && !m_text.empty()) m_highlighter(m_text, m_hcols);   // syntax colours: once per change
    }
    const std::vector<VRow>& visualRows() const { _ensureLayout(); return m_rows; }
    std::string _rowText(const VRow& r) const { return m_text.substr(r.start, r.len); }

    std::vector<std::string> getLines() const {
        std::vector<std::string> lines;
        for (const VRow& r : visualRows()) lines.push_back(_rowText(r));
        return lines;
    }

    void getCursorLineCol(size_t& outLine, size_t& outCol) const {
        const auto& rows = visualRows();
        outLine = 0; outCol = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            const size_t rowEnd = rows[r].start + rows[r].len;
            const bool last = (r + 1 == rows.size());
            if (m_cursorPos <= rowEnd || last) {                           // caret sits on this visual row
                if (!last && m_cursorPos == rowEnd + 1) continue;          // exactly on the '\n' → next row
                outLine = r;
                outCol  = m_cursorPos >= rows[r].start ? m_cursorPos - rows[r].start : 0;
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

        size_t line = 0, col = 0;
        getCursorLineCol(line, col);

        // ---- Ctrl shortcuts ----
        if (ke.ctrl) {
            if (ke.key == K::C || (ke.utf8[0]=='c'||ke.utf8[0]=='C')) {
                std::string sel = selectedText();
                if (!sel.empty()) clipboardSet(sel);
                return true;
            }
            if (ke.key == K::X || (ke.utf8[0]=='x'||ke.utf8[0]=='X')) {
                std::string sel = selectedText();
                if (!sel.empty()) {
                    clipboardSet(sel);
                    _deleteSelection();
                }
                return true;
            }
            if (ke.key == K::V || (ke.utf8[0]=='v'||ke.utf8[0]=='V')) {
                std::string clip = clipboardGet();
                if (!clip.empty()) {
                    _deleteSelection();
                    if (m_maxLen && m_text.size() + clip.size() > m_maxLen)      // truncate the paste to fit the cap
                        clip.resize(m_maxLen > m_text.size() ? m_maxLen - m_text.size() : 0);
                    if (!clip.empty()) {
                    m_text.insert(m_cursorPos, clip);
                    m_cursorPos += clip.size();
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                    }
                }
                return true;
            }
            if (ke.key == K::A || (ke.utf8[0]=='a'||ke.utf8[0]=='A')) {
                m_selStart  = 0;
                m_selEnd    = m_text.size();
                m_selActive = true;
                m_cursorPos = m_selEnd;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                return true;
            }
        }

        // ---- Movement / selection with optional Shift ----
        auto moveCursor = [&](size_t newPos) {
            if (ke.shift) {
                if (!m_selActive) { m_selStart = m_cursorPos; m_selActive = true; }
                m_selEnd    = newPos;
            } else {
                m_selActive = false;
            }
            m_cursorPos = newPos;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
        };

        if (ke.key == K::Backspace) {
            if (m_selActive && m_selStart != m_selEnd) {
                _deleteSelection();
            } else if (ke.ctrl && m_cursorPos > 0) {           // Ctrl+Backspace → delete the word to the left
                const size_t p = _prevWord(m_cursorPos);
                if (p < m_cursorPos) {
                    m_text.erase(p, m_cursorPos - p);
                    m_cursorPos = p;
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                }
            } else if (m_cursorPos > 0 && !m_text.empty()) {
                m_text.erase(m_cursorPos - 1, 1);
                m_cursorPos--;
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
            }
            return true;
        } else if (ke.key == K::Delete) {
            if (m_selActive && m_selStart != m_selEnd) {
                _deleteSelection();
            } else if (ke.ctrl && m_cursorPos < m_text.size()) {   // Ctrl+Delete → delete the word to the right
                const size_t e = _nextWord(m_cursorPos);
                if (e > m_cursorPos) {
                    m_text.erase(m_cursorPos, e - m_cursorPos);
                    m_layoutDirty = true;
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    onTextChanged.emit(m_text);
                }
            } else if (m_cursorPos < m_text.size()) {
                m_text.erase(m_cursorPos, 1);
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
            }
            return true;
        } else if (ke.key == K::Return) {
            _deleteSelection();
            if (_full()) return true;                          // at the cap → reject
            m_text.insert(m_cursorPos, "\n");
            m_cursorPos++;
            m_layoutDirty = true;
            m_graph.invalidateNode(m_nodeId, DirtySelf);
            onTextChanged.emit(m_text);
            return true;
        } else if (ke.key == K::Left) {
            if (ke.ctrl) {                                     // Ctrl(+Shift)+Left → move/select by word
                moveCursor(_prevWord(m_cursorPos));
            } else if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
                m_cursorPos = std::min(m_selStart, m_selEnd);
                m_selActive = false;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            } else {
                moveCursor(m_cursorPos > 0 ? m_cursorPos - 1 : 0);
            }
            return true;
        } else if (ke.key == K::Right) {
            if (ke.ctrl) {                                     // Ctrl(+Shift)+Right → move/select by word
                moveCursor(_nextWord(m_cursorPos));
            } else if (!ke.shift && m_selActive && m_selStart != m_selEnd) {
                m_cursorPos = std::max(m_selStart, m_selEnd);
                m_selActive = false;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
            } else {
                moveCursor(m_cursorPos < m_text.size() ? m_cursorPos + 1 : m_text.size());
            }
            return true;
        } else if (ke.key == K::Up) {
            if (line > 0) moveCursor(getPosFromLineCol(line - 1, col));
            return true;
        } else if (ke.key == K::Down) {
            auto lines = getLines();
            if (line + 1 < lines.size()) moveCursor(getPosFromLineCol(line + 1, col));
            return true;
        } else if (ke.key == K::Home) {
            moveCursor(getPosFromLineCol(line, 0));
            return true;
        } else if (ke.key == K::End) {
            auto lines = getLines();
            moveCursor(getPosFromLineCol(line, lines[line].size()));
            return true;
        } else if (ke.utf8[0] != '\0' && !ke.ctrl) {
            if (static_cast<uint8_t>(ke.utf8[0]) >= 32 || ke.utf8[0] == '\t') {
                _deleteSelection();
                if (m_maxLen && m_text.size() + std::strlen(ke.utf8) > m_maxLen) return true;   // at the cap → reject
                m_text.insert(m_cursorPos, ke.utf8);
                m_cursorPos += std::strlen(ke.utf8);
                m_layoutDirty = true;
                m_graph.invalidateNode(m_nodeId, DirtySelf);
                onTextChanged.emit(m_text);
                return true;
            }
        }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        bool focused = isFocused();

        // Background + border (accent when focused)
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface1,
                          JTheme::current().hint(JStyleHint::ControlRadius),
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

        // Scroll to keep the caret visible — ONLY when the caret just moved (typing / arrows / click), so the
        // mouse wheel can freely scroll elsewhere without the view snapping back to the caret every frame.
        if (m_ensureCaret) {
            float cursorYRel = cursorLine * lh;
            if (cursorYRel < m_scrollOffset)                 m_scrollOffset = cursorYRel;
            else if (cursorYRel + lh > m_scrollOffset + innerH) m_scrollOffset = cursorYRel + lh - innerH;
            m_ensureCaret = false;
        }
        // Always clamp to the content extent (handles text shrinking / resize).
        const float maxScroll = std::max(0.0f, static_cast<float>(rows.size()) * lh - innerH);
        m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxScroll);

        buf.pushClip(innerX, innerY, innerW, innerH);   // wrapped rows fit, but clip so nothing ever spills out

        auto rowX = [&](const std::string& t, size_t nchars) -> float {
            return innerX + (JTextHelper::hasAtlas() ? JTextHelper::measureWidth(t.substr(0, nchars))
                                                     : static_cast<float>(nchars) * 6.0f);
        };

        // Selection highlight — per visual row, the intersection of the row's byte range with [selLo, selHi).
        if (m_selActive && m_selStart != m_selEnd) {
            const size_t selLo = std::min(m_selStart, m_selEnd), selHi = std::max(m_selStart, m_selEnd);
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
            const std::vector<uint8_t>& cols = m_hcols;   // syntax colours from the cached layout (not per frame)
            const uint8_t tc[4] = {Colors::ControlText[0], Colors::ControlText[1], Colors::ControlText[2], 220};
            if (m_text.empty() && !m_placeholder.empty()) {
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
            if (m_text.empty()) {
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
            if (!m_text.empty() && cursorLine < rows.size())
                cx = rowX(_rowText(rows[cursorLine]), std::min(cursorCol, rows[cursorLine].len));
            float cy = innerY + cursorLine * lh - m_scrollOffset;
            if (cy + lh >= innerY && cy <= innerY + innerH)
                buf.pushRectangle(cx, cy + 2.0f, 1.5f, lh - 4.0f, Colors::Accent);
        }

        buf.popClip();

        // Vertical scrollbar — shown only when the content overflows. Track down the right inner edge, thumb
        // sized/positioned by the visible fraction. Drag it with dragScrollThumb via handleMousePress/Move.
        const float contentH = static_cast<float>(rows.size()) * lh;
        m_hasScrollBar = contentH > innerH + 1.0f;
        if (m_hasScrollBar) {
            const float sbW = 8.0f;
            m_sbX = b.x + b.width - sbW - 3.0f; m_sbY = b.y + 4.0f; m_sbW = sbW; m_sbTrackH = b.height - 8.0f;
            buf.pushRectangle(m_sbX, m_sbY, sbW, m_sbTrackH, Colors::Surface0, sbW * 0.5f);
            const float maxScroll = contentH - innerH;
            m_sbThumbH = std::max(24.0f, m_sbTrackH * (innerH / contentH));
            const float frac = maxScroll > 0.0f ? (m_scrollOffset / maxScroll) : 0.0f;
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


    // Optional syntax highlighter: fills `out` with 4 bytes (RGBA) per character of the text; the render then
    // draws each line as runs of equal colour. Null (default) → the whole text draws in one colour (no change
    // for any existing JTextArea). Used by the studio's Lua editor.
    void setHighlighter(std::function<void(const std::string&, std::vector<uint8_t>&)> h) { m_highlighter = std::move(h); m_graph.invalidateNode(m_nodeId, DirtySelf); }

private:
    std::function<void(const std::string&, std::vector<uint8_t>&)> m_highlighter;   // null = plain single-colour text

    // Word-boundary navigation (Ctrl+Left/Right, Ctrl+Backspace/Delete). A word char is ASCII alnum, '_' or
    // any UTF-8 byte ≥0x80; everything else is a separator. Semantics match JLineEdit / Qt / GTK.
    static bool _isWordChar(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
    }
    size_t _nextWord(size_t i) const {
        const size_t n = m_text.size();
        while (i < n &&  _isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        while (i < n && !_isWordChar(static_cast<unsigned char>(m_text[i]))) ++i;
        return i;
    }
    size_t _prevWord(size_t i) const {
        while (i > 0 && !_isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        while (i > 0 &&  _isWordChar(static_cast<unsigned char>(m_text[i - 1]))) --i;
        return i;
    }

    void _deleteSelection() {
        if (!m_selActive || m_selStart == m_selEnd) return;
        size_t lo = std::min(m_selStart, m_selEnd);
        size_t hi = std::max(m_selStart, m_selEnd);
        m_text.erase(lo, hi - lo);
        m_cursorPos = lo;
        m_selActive = false;
        m_layoutDirty = true;
        onTextChanged.emit(m_text);
    }

    std::string m_text;
    std::string m_placeholder;
    size_t      m_cursorPos{0};
    float       m_scrollOffset{0.0f};
    size_t      m_selStart{0};
    size_t      m_selEnd{0};
    bool        m_selActive{false};
    bool        m_ensureCaret{true};   // scroll to the caret next render (set on caret-moving actions)
    // Vertical scrollbar geometry, recomputed each render; used for wheel + thumb-drag hit-testing.
    bool        m_hasScrollBar{false};
    float       m_sbX{0}, m_sbY{0}, m_sbW{0}, m_sbTrackH{0}, m_sbThumbY{0}, m_sbThumbH{0};
    bool        m_sbDragging{false};
    float       m_sbGrabDY{0};
    // Cached line layout (wrapped rows + syntax colours) — see _ensureLayout(). Recomputed only on a text or
    // width change (m_layoutDirtY); the render/cursor/click just read it. `mutable` so const geometry ops
    // (called from the const render) can lazily refresh it.
    mutable std::vector<VRow>   m_rows;
    mutable std::vector<uint8_t> m_hcols;         // per-char RGBA syntax colours (empty = no highlighter)
    mutable bool                m_layoutDirty{true};
    mutable float               m_layoutW{-1.0f};
    size_t                      m_maxLen{0};          // hard character cap (0 = unlimited)
};

} // inline namespace jf
