#pragma once

// JMenuRuntime — the menu popup engine: dropdown popup windows, submenu cascades, modal
// click-outside-to-dismiss, keyboard nav, and torn-off floating menus. Ported out of the
// catalog so every app gets working menus for free. JAppWindow owns one and drives it each
// frame; the app only builds JMenu/JMenuItem trees and adds them to the menu bar.

#include <j/core/MenuSystem.h>
#include <j/platforms/PopupWindow.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#include <functional>
#include <memory>
#include <vector>

inline namespace jf {

class JMenuRuntime {
public:
    // Install the JMenuManager::onOpenMenu hook. `parentWindow` is the main window's native
    // handle (for pointer grab + focus); `bar` is reset whenever the menus close.
    void wire(JGpuHal* hal, JPopupWindow::NativeWinHandleType parentWindow, JMenuBar* bar) {
        m_hal = hal; m_parent = parentWindow; m_bar = bar;
        JMenuManager::instance().onOpenMenu = [this](JMenu* menu, int sx, int sy, bool parentTorn) {
            // Defer if we're mid-poll (a popup callback re-entered us) to avoid mutating the
            // popup list while iterating it.
            if (m_isPolling)
                m_deferred.push_back([this, menu, sx, sy, parentTorn]() { openMenu(menu, sx, sy, parentTorn); });
            else
                openMenu(menu, sx, sy, parentTorn);
        };
    }

    bool hasOpenMenus() const { return !m_active.empty(); }

    // Per-frame: poll modal popups (grab + dismiss-on-outside), run deferred callbacks, then
    // render active + floating menus. Each popup is its own window/surface, so it renders
    // into a fresh scratch buffer (like a floating dock window).
    void updateAndRender(JGpuHal& hal) {
        if (!m_active.empty()) {
            m_isPolling = true;
            // Single grab: the ROOT popup owns the pointer and receives every event; we read
            // the global cursor from it and drive the whole stack ourselves. This is what
            // makes submenu cascades work — a child can't steal the grab and freeze / dismiss
            // its parent.
            JPopupWindow* root = m_active.front().get();
            bool dismissed = root->pumpAndGrab();
            int  gx = 0, gy = 0;
            bool pressed = false, released = false;
            if (!dismissed) {
                auto gc = root->globalCursor(); gx = gc.first; gy = gc.second;
                for (const auto& ke : root->takeKeys()) {
                    if (!ke.pressed) continue;
                    if (ke.key == JKeyEvent::JKey::Escape) { dismissed = true; break; }
                    m_active.back()->handleKeyNav(ke);   // arrow nav on the topmost popup
                }
                // Pump every child popup so its native button events are read this frame, then aggregate
                // press/release across ALL popups. With several override-redirect popup windows, X delivers
                // the button event to whichever window is under the cursor (a submenu) rather than the
                // grabbing root — so reading press from the root alone silently dropped every submenu click.
                for (size_t i = 1; i < m_active.size(); ++i) m_active[i]->pumpManaged();
                for (auto& p : m_active) { if (p->takePress()) pressed = true; if (p->takeRelease()) released = true; }
            }
            if (!dismissed) {
                bool insideAny = false;
                for (auto& p : m_active) {
                    if (p->containsGlobal(gx, gy)) insideAny = true;
                    p->driveInput(static_cast<float>(gx - p->window().screenX()),
                                  static_cast<float>(gy - p->window().screenY()),
                                  pressed, released);
                }
                if (pressed && !insideAny) dismissed = true;   // clicked outside every menu
            }
            m_isPolling = false;

            if (!m_deferred.empty()) {
                auto acts = std::move(m_deferred);
                m_deferred.clear();
                for (auto& a : acts) a();
            }

            if (dismissed) closeAll();
            else for (auto& p : m_active) if (p->isViewable()) { JPrimitiveBuffer b; p->render(hal, b); }
        }

        for (auto it = m_floating.begin(); it != m_floating.end(); ) {
            if ((*it)->pollFloating() == JPopupWindow::JFloatPollResult::Close) {
                (*it)->destroySurface(hal);
                it = m_floating.erase(it);
            } else {
                JPrimitiveBuffer b; (*it)->render(hal, b);
                ++it;
            }
        }
    }

    void closeAll() {
        if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
        m_active.clear();
        if (m_bar) m_bar->closeMenu();
    }

private:
    void openMenu(JMenu* menu, int sx, int sy, bool parentTorn) {
        if (!menu) { closeAll(); return; }
        if (!parentTorn) {                     // a new top-level menu replaces the open one
            if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
            m_active.clear();
        }

        auto popup = std::make_unique<JPopupWindow>(
            sx, sy, 180, 8, *m_hal, JPopupWindow::JStyle::Bordered, m_parent, nullptr);

        // Tear-off handle (the grab-strip at the top): pressing it promotes this menu to a
        // floating, draggable, closeable window — reusing JPopupWindow's floating mode and the
        // m_floating servicing in updateAndRender (same mechanism a torn-out dock uses in spirit).
        if (menu->isTearOffEnabled() && JMenuManager::instance().isTearOffEnabled()) {
            JPopupWindow* self = popup.get();
            popup->add<JTearOffHandle>()->onTornOff.connect(
                [this, self]() { m_deferred.push_back([this, self]() { _tearOff(self); }); });
        }

        for (const auto& item : menu->items()) {
            if (!item) continue;
            if (auto* mi = dynamic_cast<JMenuItem*>(item.get())) {
                auto* added = popup->add<JMenuItem>(mi->label(), mi->shortcut(), mi->submenu());
                added->setCheckable(mi->isCheckable());
                added->setChecked(mi->isChecked());
                added->setTooltip(mi->tooltip());
                if (mi->embeddedWidgetFactory()) added->setEmbeddedWidgetFactory(mi->embeddedWidgetFactory());

                JMenuItem* src = mi;
                added->onTriggered.connect([this, src, added]() {
                    // Reflect a checkable toggle back onto the model item so the app's handler
                    // (which reads the model item's state) and a re-opened menu are correct —
                    // the popup entry is only a copy.
                    if (src->isCheckable()) src->setChecked(added->isChecked());
                    src->onTriggered.emit();
                    // Any leaf (plain or checkable) dismisses a docked menu once chosen.
                    // A floating/torn menu stays open — closeAll() only affects m_active,
                    // so its checkables keep toggling in place.
                    if (!src->submenu())
                        m_deferred.push_back([this]() { closeAll(); });
                });

                // EVERY item drives the cascade on hover: collapse anything open deeper than THIS
                // item's popup (so sibling submenus can't pile up → the old surface-id overflow
                // crash), then — submenu parents only — open this item's own submenu. Passing a
                // leaf's null submenu makes hovering a plain item CLOSE a sibling's open submenu,
                // while a leaf INSIDE a submenu only closes things deeper than itself, so navigating
                // into an open submenu keeps it (and its tear-off handle) live.
                {
                    JPopupWindow* parent = popup.get();
                    JMenuItem*    a      = added;
                    JMenu*        sub    = mi->submenu();   // nullptr for leaf items
                    added->onHoverEntered.connect([this, a, parent, sub]() { hoverItem(parent, a, sub); });
                }
            } else if (dynamic_cast<JMenuSeparator*>(item.get())) {
                popup->add<JMenuSeparator>();
            }
        }

        popup->computeNaturalHeight();
        m_active.push_back(std::move(popup));
    }

    // Hover handling for a menu item: collapse the cascade back to `parent` (destroying any sibling /
    // deeper submenu popups), then open this item's submenu if it has one. Deferred while polling so we
    // never mutate m_active mid-iteration. This is what stops submenu popups piling up on every hover
    // (which grew GPU surface IDs without bound until the text-vertex buffer overran → SIGSEGV).
    void hoverItem(JPopupWindow* parent, JMenuItem* a, JMenu* sub) {
        if (m_isPolling) { m_deferred.push_back([this, parent, a, sub]() { hoverItem(parent, a, sub); }); return; }
        int pi = -1;
        for (size_t i = 0; i < m_active.size(); ++i) if (m_active[i].get() == parent) { pi = static_cast<int>(i); break; }
        if (pi < 0) return;                                  // parent already closed
        // Nothing to do if this item's submenu is already the child directly under the parent.
        const bool alreadyOpen = sub && pi + 1 < static_cast<int>(m_active.size());
        if (m_hal) for (size_t i = pi + 1; i < m_active.size(); ++i) m_active[i]->destroySurface(*m_hal);
        m_active.erase(m_active.begin() + pi + 1, m_active.end());
        (void)alreadyOpen;
        if (sub) {
            const auto& l = parent->graph().getLayoutConst(a->getNodeId());
            const int ssx = parent->window().screenX() + static_cast<int>(l.boundingBox.x + l.boundingBox.width);
            const int ssy = parent->window().screenY() + static_cast<int>(l.boundingBox.y);
            openMenu(sub, ssx, ssy, true);
        }
    }

    // Promote a popup (the one whose tear-off handle was pressed — root OR a submenu) to a floating
    // menu: it survives with no grab, a close button and drag-to-move; the rest of the cascade closes.
    void _tearOff(JPopupWindow* which) {
        if (m_active.empty()) return;
        std::unique_ptr<JPopupWindow> torn;
        for (auto& p : m_active) if (p.get() == which) { torn = std::move(p); break; }
        if (!torn) torn = std::move(m_active.front());   // fallback: tear the root
        if (m_hal) for (auto& p : m_active) if (p) p->destroySurface(*m_hal);
        m_active.clear();
        torn->releasePointerGrab();
        torn->enableCloseButton();
        m_floating.push_back(std::move(torn));
        if (m_bar) m_bar->closeMenu();
    }

    JGpuHal*                          m_hal{nullptr};
    JPopupWindow::NativeWinHandleType m_parent{};
    JMenuBar*                         m_bar{nullptr};
    std::vector<std::unique_ptr<JPopupWindow>> m_active;     // modal dropdown stack
    std::vector<std::unique_ptr<JPopupWindow>> m_floating;   // torn-off menus
    std::vector<std::function<void()>>         m_deferred;   // run after polling
    bool                              m_isPolling{false};
};

} // inline namespace jf
