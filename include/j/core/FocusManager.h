#pragma once

// Thread-safety: MAIN THREAD ONLY.
// Focus state is driven by keyboard and mouse events on the render thread.

#include <vector>
#include <unordered_map>
#include <unordered_set>
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

    // ---- Tab order: a WALK OF THE WIDGET TREE ------------------------------------------------------
    // The focus chain belongs to a WINDOW and is derived from that window's widget tree, exactly as Qt
    // (nextInFocusChain walks parent->child) and GTK do. The host declares its tree roots once; traversal
    // descends the scene-graph hierarchy from them.
    //
    // This replaced filtering a global registry. JWidget::s_activeWidgets holds every JWidget ever
    // CONSTRUCTED -- which in a real app includes objects that are not UI: a JMenu stores its entries as
    // JMenuItem widgets, the widget registry caches a prototype per type, and a hosted control is a member
    // created lazily. None is ever parented into a window, yet all are focusable and sit at (0,0) with
    // their constructor size. Subtracting them needs an ever-growing pile of predicates (big enough? ever
    // laid out? excluded?), and each new kind of non-UI widget reintroduces the bug. Membership by TREE
    // needs none of it: never parented into the window, never in the chain.
    void setFocusRoots(std::vector<JWidget*> roots) { m_roots = std::move(roots); }
    const std::vector<JWidget*>& focusRoots() const { return m_roots; }

    // Rebuild the order by descending from the roots. Inside the tree the usual rules still apply:
    // focusable policy, visibility (a hidden node prunes its subtree), enabled, and the explicit
    // scan-exclusion a host sets on content it drives itself.
    void syncOrder() {
        m_order.clear();
        std::unordered_set<const JWidget*> seen;
        for (JWidget* root : m_roots) if (root) _collect(root, seen);
        constexpr float kRowBand = 6.0f;
        std::stable_sort(m_order.begin(), m_order.end(), [](JWidget* a, JWidget* b) {
            const auto ba = a->getBoundingBox(), bb = b->getBoundingBox();
            const float dy = ba.y - bb.y;
            if (dy < -kRowBand) return true;
            if (dy >  kRowBand) return false;
            return ba.x < bb.x;
        });
        // The focused widget left the order (its page hid, its dock tabbed behind, it was destroyed).
        // Clear it PROPERLY -- nulling the pointer alone leaves the widget flagged focused, so it keeps
        // painting a focus ring for ever and the next focus produces a second one.
        if (m_focused && std::find(m_order.begin(), m_order.end(), m_focused) == m_order.end())
            setFocus(nullptr);
    }

    // Click-to-focus over the tree: the same membership rule as Tab, so a click can never focus
    // something outside this window. Scanned back-to-front (topmost wins).
    void focusAt(float mx, float my) {
        syncOrder();
        JWidget* hit = nullptr;
        for (auto it = m_order.rbegin(); it != m_order.rend(); ++it)
            if ((*it)->hitTest(mx, my)) { hit = *it; break; }
        setFocus(hit);
    }

    // Give a freshly-opened window its initial focus: the first widget in reading order. Every toolkit
    // focuses something when a window opens — without it the keyboard does nothing until the user clicks.
    void focusFirst() {
        syncOrder();
        if (!m_order.empty()) setFocus(m_order.front());
    }

    void clear() { m_order.clear(); m_focused = nullptr; }

private:
    // Depth-first descent of the WIDGET tree (JWidget::collectChildren). A hidden or scan-excluded node
    // prunes its whole subtree: nothing inside something the user cannot see, or that the host paints and
    // hit-tests itself, is a tab stop. `seen` guards against a widget reachable by two edges (owned AND
    // registered non-owningly in a container).
    void _collect(JWidget* w, std::unordered_set<const JWidget*>& seen) {
        if (!w || !seen.insert(w).second) return;
        if (!w->isVisibleSelf() || w->isScanExcludedSelf()) return;
        if (w->isFocusable() && w->isEnabled()) m_order.push_back(w);
        std::vector<JWidget*> kids;
        w->collectChildren(kids);
        for (JWidget* k : kids) _collect(k, seen);
    }

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

    std::vector<JWidget*> m_roots;   // this window's tree roots, declared by the host
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
inline void jRouteMouse(float mx, float my, JFocusManager& focus) {
    focus.focusAt(mx, my);
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
inline bool jRouteKey(const JKeyEvent& ke, JFocusManager& focus) {
    if (!ke.pressed) return false;
    focus.syncOrder();

    if (JWidget* f = focus.focused(); f && f->handleKeyEvent(ke)) return true;
    if (ke.key == JKeyEvent::JKey::Tab)     { focus.nextFocus(); return true; }
    if (ke.key == JKeyEvent::JKey::BackTab) { focus.prevFocus(); return true; }
    return false;
}

} // inline namespace jf
