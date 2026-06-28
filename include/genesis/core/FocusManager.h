#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Focus state is driven by keyboard and mouse events on the render thread.

#include <vector>
#include <algorithm>
#include "Signal.h"

#include "BaseWidgets.h"

namespace Genesis {

#include "KeyEvent.h"

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
            old->setFocused(false);
            if (old->getState() == WidgetState::Focused) {
                old->setState(WidgetState::Normal);
            }
        }
        if (m_focused) {
            m_focused->setFocused(true);
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
