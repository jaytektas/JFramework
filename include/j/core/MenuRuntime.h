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
                pressed  = root->takePress();
                released = root->takeRelease();
                for (const auto& ke : root->takeKeys()) {
                    if (!ke.pressed) continue;
                    if (ke.key == JKeyEvent::JKey::Escape) { dismissed = true; break; }
                    m_active.back()->handleKeyNav(ke);   // arrow nav on the topmost popup
                }
            }
            if (!dismissed) {
                bool insideAny = false;
                for (auto& p : m_active) {
                    p->pumpManaged();
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
            popup->add<JTearOffHandle>()->onTornOff.connect(
                [this]() { m_deferred.push_back([this]() { _tearOff(); }); });
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
                added->onTriggered.connect([this, src]() {
                    src->onTriggered.emit();
                    if (!src->submenu())           // leaf item: close the whole stack
                        m_deferred.push_back([this]() { closeAll(); });
                });

                if (mi->submenu()) {               // cascade: open the submenu on hover
                    JPopupWindow* parent = popup.get();
                    JMenuItem*    a      = added;
                    added->onHoverEntered.connect([a, parent]() {
                        const auto& l = parent->graph().getLayoutConst(a->getNodeId());
                        int ssx = parent->window().screenX() + static_cast<int>(l.boundingBox.x + l.boundingBox.width);
                        int ssy = parent->window().screenY() + static_cast<int>(l.boundingBox.y);
                        JMenuManager::instance().onOpenMenu(a->submenu(), ssx, ssy, true);
                    });
                }
            } else if (dynamic_cast<JMenuSeparator*>(item.get())) {
                popup->add<JMenuSeparator>();
            }
        }

        popup->computeNaturalHeight();
        m_active.push_back(std::move(popup));
    }

    // Promote the open menu's root popup to a floating menu. Submenus (if any) are dropped —
    // only the torn menu survives, now with no grab, a close button, and drag-to-move (all
    // provided by JPopupWindow's floating mode; serviced by the m_floating loop above).
    void _tearOff() {
        if (m_active.empty()) return;
        auto root = std::move(m_active.front());
        m_active.erase(m_active.begin());
        if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
        m_active.clear();
        root->releasePointerGrab();
        root->enableCloseButton();
        m_floating.push_back(std::move(root));
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
