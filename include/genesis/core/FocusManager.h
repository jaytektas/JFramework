#pragma once

#include <vector>
#include <algorithm>
#include "Signal.h"

#include "BaseWidgets.h"

namespace Genesis {

/** Keyboard input event passed from platform to the app each frame. */
struct KeyEvent {
    enum class Key : uint32_t {
        Unknown = 0,
        Tab, BackTab,       // focus navigation
        Return, Space,      // activation
        Escape,
        Backspace, Delete,
        Left, Right, Up, Down,
        Home, End,
        A=65, B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,
        _0=48,_1,_2,_3,_4,_5,_6,_7,_8,_9,
    };
    Key      key{Key::Unknown};
    uint32_t keysym{0};
    char     utf8[8]{};     // UTF-8 text for printable characters
    bool     shift{false}, ctrl{false}, alt{false};
    bool     pressed{true}; // false on release
};

/**
 * @brief FocusManager — tracks which widget has keyboard focus and handles
 * Tab/Shift-Tab cycling.
 *
 * The app registers every focusable widget (Controls) at construction time.
 * The render loop calls advance(key) on Tab/Shift-Tab, and deliverKey(widget, key)
 * for the currently focused widget's activation (Enter/Space).
 *
 * Accessibility note: every focus change emits onFocusChanged which the
 * AT-SPI bridge listens to for object:state-changed:focused signals.
 */
class FocusManager {
public:
    FocusManager() = default;

    Core::Signal<Widget*> onFocusChanged; // nullptr = focus cleared

    void registerWidget(Widget* w) {
        if (w && std::find(m_order.begin(), m_order.end(), w) == m_order.end())
            m_order.push_back(w);
    }

    void unregisterWidget(Widget* w) {
        m_order.erase(std::remove(m_order.begin(), m_order.end(), w), m_order.end());
        if (m_focused == w) setFocus(nullptr);
    }

    void setFocus(Widget* w) {
        if (m_focused == w) return;
        Widget* old = m_focused;
        m_focused = w;
        if (old) {
            if (old->getState() == WidgetState::Focused) {
                old->setState(WidgetState::Normal);
            }
        }
        if (m_focused) {
            m_focused->setState(WidgetState::Focused);
        }
        onFocusChanged.emit(w);
    }

    Widget* focused() const { return m_focused; }

    bool isFocused(const Widget* w) const { return m_focused == w; }

    void nextFocus() { _shift(+1); }
    void prevFocus() { _shift(-1); }

    void clear() { m_order.clear(); m_focused = nullptr; }

private:
    void _shift(int dir) {
        if (m_order.empty()) return;
        int sz  = static_cast<int>(m_order.size());
        int cur = 0;
        if (m_focused) {
            auto it = std::find(m_order.begin(), m_order.end(), m_focused);
            if (it != m_order.end()) cur = static_cast<int>(it - m_order.begin());
        }
        int next = ((cur + dir) % sz + sz) % sz;
        setFocus(m_order[next]);
    }

    std::vector<Widget*> m_order;
    Widget*              m_focused{nullptr};
};

} // namespace Genesis
