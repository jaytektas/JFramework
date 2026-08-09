#pragma once

// JMenuRuntime — the menu popup engine: dropdown popup windows, submenu cascades, modal
// click-outside-to-dismiss, keyboard nav, and torn-off floating menus. Ported out of the
// catalog so every app gets working menus for free. JAppWindow owns one and drives it each
// frame; the app only builds JMenu/JMenuItem trees and adds them to the menu bar.

#include <j/core/MenuSystem.h>
#include <j/platforms/PopupWindow.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

inline namespace jf {

class JMenuRuntime {
public:
    // Install the JMenuManager::onOpenMenu hook. `parentWindow` is the main window's native
    // handle (for pointer grab + focus); `bar` is reset whenever the menus close.
    void wire(JGpuHal* hal, JPopupWindow::NativeWinHandleType parentWindow, JMenuBar* bar) {
        m_hal = hal; m_parent = parentWindow; m_bar = bar;
        JMenuManager::instance().onOpenMenu = [this](JMenu* menu, int sx, int sy, bool parentTorn, bool pointAnchored) {
            // Defer if we're mid-poll (a popup callback re-entered us) to avoid mutating the
            // popup list while iterating it.
            // A point-anchored menu flips about the click; an anchored dropdown only slides (kNoFlip).
            const int fx = pointAnchored ? sx : kNoFlip, fy = pointAnchored ? sy : kNoFlip;
            if (m_isPolling)
                m_deferred.push_back([this, menu, sx, sy, parentTorn, fx, fy]() { openMenu(menu, sx, sy, parentTorn, fx, fy); });
            else
                openMenu(menu, sx, sy, parentTorn, fx, fy);
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

        if (!m_floating.empty()) {
            // Poll every floating popup (torn root or submenu child) for its own events. Hover fires the cascade
            // via hoverItem → hoverFloating, DEFERRED (m_isPolling), so the m_floating list is never mutated mid-
            // iteration; the deferred actions (open/close submenus) run afterwards, then closes are applied.
            m_isPolling = true;
            std::vector<JPopupWindow*> closing;
            for (auto& fn : m_floating)
                if (fn.win->pollFloating() == JPopupWindow::JFloatPollResult::Close) closing.push_back(fn.win.get());
            m_isPolling = false;

            if (!m_deferred.empty()) {
                auto acts = std::move(m_deferred);
                m_deferred.clear();
                for (auto& a : acts) a();
            }
            for (auto* w : closing) closeFloatingSubtree(w, /*includeRoot=*/true);   // a closed menu takes its submenus with it

            for (auto& fn : m_floating)
                if (fn.win->isViewable()) { JPrimitiveBuffer b; fn.win->render(hal, b); }
        }
    }

    void closeAll() {
        if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
        m_active.clear();
        if (m_bar) m_bar->closeMenu();
    }

private:
    // "This axis has nothing to flip about" — the popup may only slide back inside the work area.
    static constexpr int kNoFlip = std::numeric_limits<int>::min();

    void openMenu(JMenu* menu, int sx, int sy, bool parentTorn, int flipX = kNoFlip, int flipY = kNoFlip) {
        if (!menu) { closeAll(); return; }
        if (!parentTorn) {                     // a new top-level menu replaces the open one
            if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
            m_active.clear();
        }
        m_active.push_back(buildMenuPopup(menu, sx, sy, /*tearOffHandle=*/true, flipX, flipY));
    }

    // Build a popup window for a menu: its items (with hover-cascade + trigger wiring) and, when asked, the
    // tear-off grab-strip. Shared by the modal dropdown stack (openMenu) and torn-off submenu cascades
    // (hoverFloating), so both behave identically — the only difference is which list owns the popup.
    std::unique_ptr<JPopupWindow> buildMenuPopup(JMenu* menu, int sx, int sy, bool tearOffHandle,
                                                 int flipX = kNoFlip, int flipY = kNoFlip) {
        auto popup = std::make_unique<JPopupWindow>(
            sx, sy, 180, 8, *m_hal, JPopupWindow::JStyle::Bordered, m_parent, nullptr);

        // Tear-off handle (the grab-strip at the top): pressing it promotes this menu to a floating,
        // draggable, closeable window. Only the modal stack offers it — a submenu of an already-floating menu
        // is served floating already, and its handle would tear from the wrong (m_active) list.
        if (tearOffHandle && menu->isTearOffEnabled() && JMenuManager::instance().isTearOffEnabled()) {
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
                // …and whether it can be chosen. The popup entry is a COPY of the model item, and every
                // property that decides how it behaves has to come across or the copy silently disagrees
                // with the model: a disabled item rendered normal and stayed clickable, so an app that
                // greyed an option (a pin another sensor holds) still let you pick it.
                added->setEnabled(mi->isEnabled());
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
        // Keep it on-screen, once its natural size is known: FLIP, then SLIDE, then CLAMP — the standard
        // order. Flipping (opening leftward/upward about the anchor) is what preserves the relationship
        // between menu and anchor: a cursor menu keeps the pointer on its corner instead of sliding items
        // underneath it, and a submenu opens on the parent's other side instead of sliding back ON TOP of
        // the parent item it belongs to. Only when the flipped side has no room either does it slide, and
        // clamping is the last resort. Measured against the WORK AREA (panels excluded), not the raw screen.
        {
            const auto wa = popup->window().workAreaAt(sx, sy);
            const int w = static_cast<int>(popup->width()), h = static_cast<int>(popup->height());
            int x = sx, y = sy;
            if (x + w > wa.x + wa.w) {
                if (flipX != kNoFlip && flipX - w >= wa.x) x = flipX - w;      // flip: right edge on the anchor
                else                                      x = wa.x + wa.w - w; // slide back inside
            }
            if (y + h > wa.y + wa.h) {
                if (flipY != kNoFlip && flipY - h >= wa.y) y = flipY - h;      // flip: bottom edge on the anchor
                else                                      y = wa.y + wa.h - h;
            }
            x = std::max(wa.x, x); y = std::max(wa.y, y);   // clamp (a menu taller/wider than the work area)
            if (x != sx || y != sy) popup->window().setPosition(x, y);
        }
        return popup;
    }

    // Where item `a`'s submenu opens: preferred top-left, plus the edges to flip about if it doesn't fit.
    // Preferred is the item's top-right, pulled back by kSubOverlap so the submenu overlaps its parent by a
    // couple of px — the diagonal mouse path to the submenu then never crosses a gap that would count as
    // leaving the item. flipX is the parent popup's LEFT edge (+overlap), so a submenu with no room on the
    // right opens leftward instead of sliding back over the parent menu; flipY is the item's BOTTOM, so one
    // near the screen bottom rises with its bottom edge on the item rather than covering the whole column.
    struct SubAnchor { int x, y, flipX, flipY; };
    SubAnchor _submenuAnchor(JPopupWindow* parent, JMenuItem* a) const {
        constexpr int kSubOverlap = 2;
        const auto& l = parent->graph().getLayoutConst(a->getNodeId());
        const int px = parent->window().screenX(), py = parent->window().screenY();
        return SubAnchor{
            px + static_cast<int>(l.boundingBox.x + l.boundingBox.width) - kSubOverlap,
            py + static_cast<int>(l.boundingBox.y),
            px + static_cast<int>(l.boundingBox.x) + kSubOverlap,
            py + static_cast<int>(l.boundingBox.y + l.boundingBox.height),
        };
    }

    // Hover handling for a menu item: collapse the cascade back to `parent` (destroying any sibling /
    // deeper submenu popups), then open this item's submenu if it has one. Deferred while polling so we
    // never mutate m_active mid-iteration. This is what stops submenu popups piling up on every hover
    // (which grew GPU surface IDs without bound until the text-vertex buffer overran → SIGSEGV).
    void hoverItem(JPopupWindow* parent, JMenuItem* a, JMenu* sub) {
        if (m_isPolling) { m_deferred.push_back([this, parent, a, sub]() { hoverItem(parent, a, sub); }); return; }
        // Modal dropdown cascade (the grabbed m_active stack).
        int pi = -1;
        for (size_t i = 0; i < m_active.size(); ++i) if (m_active[i].get() == parent) { pi = static_cast<int>(i); break; }
        if (pi >= 0) {
            if (m_hal) for (size_t i = pi + 1; i < m_active.size(); ++i) m_active[i]->destroySurface(*m_hal);
            m_active.erase(m_active.begin() + pi + 1, m_active.end());
            if (sub) {
                const auto [ssx, ssy, fx, fy] = _submenuAnchor(parent, a);
                openMenu(sub, ssx, ssy, true, fx, fy);
            }
            return;
        }
        // Torn-off (floating) cascade — the parent is a floating popup, not on the modal stack.
        if (floatingContains(parent)) hoverFloating(parent, a, sub);
    }

    bool floatingContains(JPopupWindow* p) const {
        for (const auto& fn : m_floating) if (fn.win.get() == p) return true;
        return false;
    }

    // Hovering an item in a torn-off menu (or one of its submenus): collapse whatever submenu chain that popup
    // had open, then — for a submenu parent — open this item's submenu as a floating child anchored to its right.
    // Mirrors the m_active cascade, but on the owner-linked m_floating tree.
    void hoverFloating(JPopupWindow* parent, JMenuItem* a, JMenu* sub) {
        closeFloatingSubtree(parent, /*includeRoot=*/false);   // drop parent's currently-open submenu chain
        if (!sub) return;
        const auto [ssx, ssy, fx, fy] = _submenuAnchor(parent, a);
        m_floating.push_back({ buildMenuPopup(sub, ssx, ssy, /*tearOffHandle=*/false, fx, fy), parent, a });
    }

    // Destroy `root`'s floating submenu descendants (owner chain), optionally `root` itself. Used to collapse a
    // submenu when the pointer moves to a sibling, and to close a whole torn-off menu (root + its open submenus).
    void closeFloatingSubtree(JPopupWindow* root, bool includeRoot) {
        std::vector<JPopupWindow*> kill, frontier{ root };
        if (includeRoot) kill.push_back(root);
        while (!frontier.empty()) {
            JPopupWindow* cur = frontier.back(); frontier.pop_back();
            for (auto& fn : m_floating) if (fn.owner == cur) { kill.push_back(fn.win.get()); frontier.push_back(fn.win.get()); }
        }
        if (kill.empty()) return;
        if (m_hal) for (auto* w : kill) w->destroySurface(*m_hal);
        m_floating.erase(std::remove_if(m_floating.begin(), m_floating.end(),
            [&](FloatNode& fn) { return std::find(kill.begin(), kill.end(), fn.win.get()) != kill.end(); }),
            m_floating.end());
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
        m_floating.push_back({ std::move(torn), nullptr, nullptr });   // a torn-off root: no owner
        if (m_bar) m_bar->closeMenu();
    }

    // A floating popup: a torn-off root (owner == nullptr) or one of its transient submenu children
    // (owner = the floating popup that spawned it). The owner links form the cascade tree used to collapse a
    // menu's open submenu when the pointer moves to a sibling — the floating-menu equivalent of the m_active stack.
    struct FloatNode {
        std::unique_ptr<JPopupWindow> win;
        JPopupWindow*                 owner{nullptr};
        JMenuItem*                    ownerItem{nullptr};
    };

    JGpuHal*                          m_hal{nullptr};
    JPopupWindow::NativeWinHandleType m_parent{};
    JMenuBar*                         m_bar{nullptr};
    std::vector<std::unique_ptr<JPopupWindow>> m_active;     // modal dropdown stack
    std::vector<FloatNode>                     m_floating;   // torn-off menus + their submenu cascades
    std::vector<std::function<void()>>         m_deferred;   // run after polling
    bool                              m_isPolling{false};
};

} // inline namespace jf
