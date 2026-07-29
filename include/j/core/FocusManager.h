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
        // Save the previous active target so managers NEST: a dialog constructs its own manager (becoming the
        // active focus target while open) and restores the main window's on destruction. Without this, closing
        // any dialog would null s_active and leave the main window unable to focus anything.
        m_prevActive = s_active;
        s_active = this;
        // Install the framework focus hook ONCE so a clicked control can claim focus authoritatively — it reads
        // s_active live, so it always routes to the currently-active (top-most) manager. Focusing a clicked
        // widget is the framework's job, wired here, not by any app.
        if (!JWidget::s_focusHook)
            JWidget::s_focusHook = [](JWidget* w) { if (s_active) s_active->setFocus(w); };
    }
    ~JFocusManager() {
        if (s_active == this) s_active = m_prevActive;   // restore the manager we displaced (the hook follows s_active)
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
            // Bring it into view: every ancestor gets the chance to reveal it (a scroll area scrolls to
            // it). Without this, Tab can move focus onto a control clipped outside its viewport and the
            // focus ring simply disappears off the form.
            for (JWidget* p = m_focused->parentWidget(); p; p = p->parentWidget())
                p->revealChild(m_focused);
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
    // `only` scopes the order to one scene graph — a modal dialog owns its own graph, and
    // JWidget::s_activeWidgets is app-global, so without it Tab would walk out of the dialog and
    // into the window behind. nullptr = every widget (the main runner's case).
    void syncOrder(const std::vector<JWidget*>& widgets, const JSceneGraph* only = nullptr) {
        m_order.clear();
        for (auto* w : widgets)
            if (w && w->isFocusable() && w->isVisible() && w->isEnabled() && !w->isScanExcluded() &&
                (!only || &w->sceneGraph() == only))
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
            if (w && w->isVisible() && w->isFocusable() && !w->isScanExcluded() && w->hitTest(mx, my)) { hit = w; break; }
        }
        setFocus(hit);
    }

    // Give a freshly-opened window its initial focus: the first widget in reading order. Every toolkit
    // focuses something when a window opens — without it the keyboard does nothing until the user clicks.
    void focusFirst(const std::vector<JWidget*>& widgets, const JSceneGraph* only = nullptr) {
        syncOrder(widgets, only);
        if (!m_order.empty()) setFocus(m_order.front());
    }

    void clear() { m_order.clear(); m_focused = nullptr; }

private:
    void _shift(int dir) {
        if (m_order.empty()) return;
        const int sz = static_cast<int>(m_order.size());
        // Nothing focused yet (fresh window, or the focused widget dropped out of the order): Tab starts
        // at the FIRST widget and Shift-Tab at the last. Previously cur defaulted to 0 and Tab moved to
        // index 1, silently skipping the first control.
        auto it = m_focused ? std::find(m_order.begin(), m_order.end(), m_focused) : m_order.end();
        if (it == m_order.end()) { setFocus(m_order[dir >= 0 ? 0 : sz - 1]); return; }
        const int cur  = static_cast<int>(it - m_order.begin());
        const int next = ((cur + dir) % sz + sz) % sz;      // wraps last -> first and first -> last
        setFocus(m_order[next]);
    }

    std::vector<JWidget*> m_order;
    JWidget*              m_focused{nullptr};
    JFocusManager*        m_prevActive{nullptr};   // the manager this one displaced as s_active (restored on dtor)
};

// ---- jRouteMouse ------------------------------------------------------------------------------
// Click-to-focus, for any window that hosts widgets. Call on a mouse PRESS, before dispatching the
// press to whatever is under the cursor: the topmost focusable, visible widget there takes focus,
// and clicking empty space clears it. The main runner already does this (JAppWindow), so controls
// focus on click there; a modal dialog that routes its own mouse must call this or NOTHING in it
// will ever focus by clicking — no control should have to request focus for itself.
//
// `graph` scopes the hit-test to one dialog's widgets, exactly as in jRouteKey.
inline void jRouteMouse(float mx, float my, JFocusManager& focus, const JSceneGraph* graph = nullptr) {
    if (!graph) { focus.focusAt(JWidget::s_activeWidgets, mx, my); return; }
    std::vector<JWidget*> scoped;
    scoped.reserve(JWidget::s_activeWidgets.size());
    for (auto* w : JWidget::s_activeWidgets)
        if (w && &w->sceneGraph() == graph) scoped.push_back(w);
    focus.focusAt(scoped, mx, my);
}

// ---- jRouteKey --------------------------------------------------------------------------------
// THE standard keyboard routing for any window that hosts widgets: refresh the tab order from the
// live widget set, give the focused widget first refusal, then honour Tab / Shift-Tab focus
// traversal. Tab moving between controls is a toolkit fundamental, not per-window behaviour — so it
// lives here and every host (the main runner AND every modal dialog) routes through this one
// function rather than reimplementing a key loop. Returns true when the key was consumed.
//
// `graph` scopes traversal to one dialog's widgets (see syncOrder); pass nullptr for the main window.
// The caller keeps whatever else it needs (Escape to dismiss, accelerators) AFTER this returns false.
inline bool jRouteKey(const JKeyEvent& ke, JFocusManager& focus, const JSceneGraph* graph = nullptr) {
    if (!ke.pressed) return false;
    focus.syncOrder(JWidget::s_activeWidgets, graph);

    if (JWidget* f = focus.focused(); f && f->handleKeyEvent(ke)) return true;
    if (ke.key == JKeyEvent::JKey::Tab)     { focus.nextFocus(); return true; }
    if (ke.key == JKeyEvent::JKey::BackTab) { focus.prevFocus(); return true; }
    return false;
}

} // inline namespace jf
