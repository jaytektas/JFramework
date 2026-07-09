#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Focus state is driven by keyboard and mouse events on the render thread.

#include <vector>
#include <algorithm>
#include "Signal.h"

#include "JWidget.h"

inline namespace jf {

#include "KeyEvent.h"

/**
 * @brief JFocusManager — tracks which widget has keyboard focus and handles
 * Tab/Shift-Tab cycling.
 *
 * The app registers every focusable widget (Controls) at construction time.
 * The render loop calls advance(key) on Tab/Shift-Tab, and deliverKey(widget, key)
 * for the currently focused widget's activation (Enter/Space).
 *
 * Accessibility note: every focus change emits onFocusChanged which the
 * AT-SPI bridge listens to for object:state-changed:focused signals.
 */
class JFocusManager {
public:
    // The most-recently-constructed manager is the active focus target for widget-initiated focus
    // requests (JWidget::requestFocus). The main window owns one for the app's lifetime.
    inline static JFocusManager* s_active = nullptr;

    JFocusManager() {
        s_active = this;
        // Install the framework focus hook so a clicked control can claim focus authoritatively —
        // focusing a clicked widget is the framework's job, wired here once, not by any app.
        JWidget::s_focusHook = [](JWidget* w) { if (s_active) s_active->setFocus(w); };
    }
    ~JFocusManager() {
        if (s_active == this) { s_active = nullptr; JWidget::s_focusHook = nullptr; }
    }

    jf::JSignal<JWidget*> onFocusChanged; // nullptr = focus cleared

    void registerWidget(JWidget* w) {
        if (w && std::find(m_order.begin(), m_order.end(), w) == m_order.end())
            m_order.push_back(w);
    }

    void unregisterWidget(JWidget* w) {
        m_order.erase(std::remove(m_order.begin(), m_order.end(), w), m_order.end());
        if (m_focused == w) setFocus(nullptr);
    }

    void setFocus(JWidget* w) {
        if (m_focused == w) return;
        JWidget* old = m_focused;
        m_focused = w;
        if (old) {
            old->setFocused(false);
            if (old->getState() == JWidgetState::Focused) {
                old->setState(JWidgetState::Normal);
            }
        }
        if (m_focused) {
            m_focused->setFocused(true);
            m_focused->setState(JWidgetState::Focused);
        }
        onFocusChanged.emit(w);
    }

    JWidget* focused() const { return m_focused; }

    bool isFocused(const JWidget* w) const { return m_focused == w; }

    void nextFocus() { _shift(+1); }
    void prevFocus() { _shift(-1); }

    // Rebuild the tab order from a live widget set (e.g. JWidget::s_activeWidgets), keeping
    // only focusable, visible, enabled entries and preserving the current focus. Lets a runner
    // own focus without the app registering widgets by hand; a focused widget that has since been
    // removed / destroyed / hidden is dropped safely (no dangling pointer in the order).
    //
    // Tab traverses in READING ORDER — top-to-bottom, then left-to-right by on-screen geometry —
    // not the order widgets happened to be constructed in. Widgets whose top edges fall within one
    // row band (kRowBand px) are treated as the same row and ordered left-to-right, so a horizontal
    // row of fields tabs across before dropping to the next row, matching every commercial toolkit.
    void syncOrder(const std::vector<JWidget*>& widgets) {
        m_order.clear();
        for (auto* w : widgets)
            if (w && w->isFocusable() && w->isVisible() && w->isEnabled())
                m_order.push_back(w);
        constexpr float kRowBand = 6.0f;
        std::stable_sort(m_order.begin(), m_order.end(), [](JWidget* a, JWidget* b) {
            const auto ba = a->getBoundingBox(), bb = b->getBoundingBox();
            const float dy = ba.y - bb.y;
            if (dy < -kRowBand) return true;    // a clearly above b
            if (dy >  kRowBand) return false;   // a clearly below b
            return ba.x < bb.x;                 // same row band → left-to-right
        });
        if (m_focused && std::find(m_order.begin(), m_order.end(), m_focused) == m_order.end())
            m_focused = nullptr;
    }

    // Focus the topmost focusable+visible widget under (mx,my) from `widgets` (paint order,
    // so scanned back-to-front), or clear focus if none is hit. Lets a runner do click-to-focus
    // without itself walking the widget set / hit-testing.
    void focusAt(const std::vector<JWidget*>& widgets, float mx, float my) {
        JWidget* hit = nullptr;
        for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
            JWidget* w = *it;
            if (w && w->isVisible() && w->isFocusable() && w->hitTest(mx, my)) { hit = w; break; }
        }
        setFocus(hit);
    }

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

    std::vector<JWidget*> m_order;
    JWidget*              m_focused{nullptr};
};

} // inline namespace jf
