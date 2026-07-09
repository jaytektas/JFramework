#pragma once

// JToolBar — a lightweight, framework-owned toolbar: a horizontal row of text buttons (with
// separators) that hover-highlight and fire a callback on click. JAppWindow owns one and
// drives its layout/input/render; the app just declares buttons.

#include <j/core/JTextHelper.h>
#include <j/core/JWidget.h>
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
    // Host an arbitrary widget in the bar (an icon button, status indicator, combo, …). The bar
    // lays it into a slot `width` wide (default = a square the bar's height), renders it via its
    // own populateRenderPrimitives, and routes input to it. The widget is non-owning.
    void addWidget(JWidget* w, float width = 0.f) {
        Item it{}; it.widget = w; it.w = width; it.h = w ? w->bounds().height : 0.f;  // capture natural height
        m_items.push_back(std::move(it));
    }
    bool empty() const { return m_items.empty(); }

    void setRect(JRect r) { m_rect = r; }

    // Hover + click. Returns true if the cursor is within the toolbar (caller swallows input).
    bool handleMouse(float mx, float my, bool pressed, bool released) {
        layout();
        const bool inside = (mx >= m_rect.x && mx < m_rect.x + m_rect.width &&
                             my >= m_rect.y && my < m_rect.y + m_rect.height);
        m_hover = -1;
        for (size_t i = 0; i < m_items.size(); ++i) {
            auto& it = m_items[i];
            if (it.widget) {                       // hosted widget: route input; it hit-tests itself
                it.widget->handleMouseMove(mx, my);
                if (pressed)  it.widget->handleMousePress(mx, my);
                if (released) it.widget->handleMouseRelease(mx, my);
                continue;
            }
            if (m_hover < 0 && inside && !it.sep && mx >= it.x && mx < it.x + it.w)
                m_hover = static_cast<int>(i);
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
            if (it.widget) {                                   // hosted widget: centred at its natural height
                const float wh = it.h > 0.f ? it.h : (h - 8.f);
                it.widget->setBounds({ it.x, m_rect.y + (h - wh) * 0.5f, it.w, wh });
                it.widget->populateRenderPrimitives(buf);
                continue;
            }
            // Text button: a persistent bordered chip at the scheme button height, centred vertically —
            // matches the original studio's QToolButton (panel fill + 1px border + rounded corners).
            const float bh = JTheme::current().buttonHeight;
            const float by = m_rect.y + (h - bh) * 0.5f;
            const float br = JTheme::current().cornerRadius;
            const bool  press = static_cast<int>(i) == m_pressed && static_cast<int>(i) == m_hover;
            const bool  hover = static_cast<int>(i) == m_hover;
            const uint8_t* fill = press ? Colors::AccentPress : hover ? Colors::Surface3 : Colors::Surface2;
            buf.pushRectangle(it.x, by, it.w, bh, fill, br, JTheme::current().borderWidth, Colors::Border);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4]; std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, tc);
                float tw = JTextHelper::measureWidth(it.label);
                JTextHelper::pushText(buf, it.x + (it.w - tw) * 0.5f,
                                      by + (bh - JTextHelper::lineHeight()) * 0.5f, it.label, tc);
            }
        }
    }

private:
    void layout() {
        float x = m_rect.x + kPad;
        for (auto& it : m_items) {
            if (it.sep) { it.x = x; it.w = kSep; x += kSep; continue; }
            if (it.widget) {                                   // widget slot: given width, or a square
                if (it.w <= 0.f) it.w = m_rect.height - 6.f;
                it.x = x; x += it.w + kGap; continue;
            }
            float tw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(it.label)
                                               : static_cast<float>(it.label.size()) * 8.f;
            it.w = tw + kBtnPad * 2.f;
            it.x = x;
            x += it.w + kGap;
        }
    }

    struct Item { std::string label; std::function<void()> onClick; bool sep{false}; float x{0}, w{0}, h{0}; JWidget* widget{nullptr}; };
    std::vector<Item> m_items;
    JRect m_rect{};
    int   m_hover{-1}, m_pressed{-1};
    static constexpr float kPad = 8.f, kGap = 4.f, kBtnPad = 12.f, kSep = 13.f;
};

} // inline namespace jf
