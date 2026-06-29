#pragma once

// JToolBar — a lightweight, framework-owned toolbar: a horizontal row of text buttons (with
// separators) that hover-highlight and fire a callback on click. JAppWindow owns one and
// drives its layout/input/render; the app just declares buttons.

#include <j/core/BaseWidgets.h>          // JTextHelper, Colors
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

inline namespace jf {

class JToolBar {
public:
    void addButton(std::string label, std::function<void()> onClick = {}) {
        m_items.push_back({ std::move(label), std::move(onClick), false, 0.f, 0.f });
    }
    void addSeparator() { m_items.push_back({ {}, {}, true, 0.f, 0.f }); }
    bool empty() const { return m_items.empty(); }

    void setRect(JRect r) { m_rect = r; }

    // Hover + click. Returns true if the cursor is within the toolbar (caller swallows input).
    bool handleMouse(float mx, float my, bool pressed, bool released) {
        layout();
        const bool inside = (mx >= m_rect.x && mx < m_rect.x + m_rect.width &&
                             my >= m_rect.y && my < m_rect.y + m_rect.height);
        m_hover = -1;
        if (inside)
            for (size_t i = 0; i < m_items.size(); ++i)
                if (!m_items[i].sep && mx >= m_items[i].x && mx < m_items[i].x + m_items[i].w) {
                    m_hover = static_cast<int>(i);
                    break;
                }
        if (pressed) m_pressed = m_hover;
        if (released) {
            if (m_pressed >= 0 && m_pressed == m_hover && m_items[m_pressed].onClick)
                m_items[m_pressed].onClick();
            m_pressed = -1;
        }
        return inside;
    }

    void render(JPrimitiveBuffer& buf) {
        layout();
        const float h = m_rect.height;
        for (size_t i = 0; i < m_items.size(); ++i) {
            const auto& it = m_items[i];
            if (it.sep) {
                uint8_t s[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], 160};
                buf.pushRectangle(it.x + kSep * 0.5f, m_rect.y + 8.f, 1.f, h - 16.f, s, 0.f);
                continue;
            }
            if (static_cast<int>(i) == m_pressed && static_cast<int>(i) == m_hover) {
                uint8_t c[4] = {Colors::AccentPress[0], Colors::AccentPress[1], Colors::AccentPress[2], 255};
                buf.pushRectangle(it.x + 3.f, m_rect.y + 5.f, it.w - 6.f, h - 10.f, c, 6.f);
            } else if (static_cast<int>(i) == m_hover) {
                uint8_t c[4] = {Colors::Surface2[0], Colors::Surface2[1], Colors::Surface2[2], 255};
                buf.pushRectangle(it.x + 3.f, m_rect.y + 5.f, it.w - 6.f, h - 10.f, c, 6.f);
            }
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
                float tw = JTextHelper::measureWidth(it.label);
                JTextHelper::pushText(buf, it.x + (it.w - tw) * 0.5f,
                                      m_rect.y + (h - JTextHelper::lineHeight()) * 0.5f, it.label, tc);
            }
        }
    }

private:
    void layout() {
        float x = m_rect.x + kPad;
        for (auto& it : m_items) {
            if (it.sep) { it.x = x; it.w = kSep; x += kSep; continue; }
            float tw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(it.label)
                                               : static_cast<float>(it.label.size()) * 8.f;
            it.w = tw + kBtnPad * 2.f;
            it.x = x;
            x += it.w + kGap;
        }
    }

    struct Item { std::string label; std::function<void()> onClick; bool sep; float x, w; };
    std::vector<Item> m_items;
    JRect m_rect{};
    int   m_hover{-1}, m_pressed{-1};
    static constexpr float kPad = 8.f, kGap = 4.f, kBtnPad = 12.f, kSep = 13.f;
};

} // inline namespace jf
