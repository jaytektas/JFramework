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
    // Did focus leave the APPLICATION? An open menu is an override-redirect window: it floats above every
    // other application and no window manager will take it away, so switching away by keyboard left it
    // hanging over whatever you switched to. Dismissal used to rely entirely on the pointer grab delivering
    // a click elsewhere, which never happens on an alt-tab. FocusOut alone cannot answer this — a menu
    // opening its own submenu is also a focus change — so ASK where focus actually is and compare it with
    // our own windows.
    bool _focusIsOurs() const {
        const JPopupWindow* any = !m_active.empty()   ? m_active.front().get()
                                : !m_floating.empty() ? m_floating.front().win.get()
                                                      : nullptr;
        if (!any) return true;
        const uintptr_t focus = any->window().focusedWindowRaw();
        if (focus == 0) return true;                                  // not reported: assume ours
        if (focus == static_cast<uintptr_t>(m_parent)) return true;   // the main window
        for (const auto& p : m_active)   if (focus == p->window().rawWindowId()) return true;
        for (const auto& f : m_floating) if (focus == f.win->window().rawWindowId()) return true;
        return false;
    }

    // NOTE on the transition rule used above: dismissal keys off LOSING focus, never off the mere state of
    // not having it. Some setups never give this process the input focus at all (a bare X server with no
    // window manager, or focus-follows-mouse landing elsewhere), and treating "not focused" as "put it
    // away" there hides every menu on the frame it opens — the menu bar stops working entirely.

    void updateAndRender(JGpuHal& hal) {
        // Leaving the application: the modal cascade is DISMISSED (a dropdown you walked away from is
        // finished with), but a TORN-OFF menu is a palette the user deliberately kept — so it is hidden,
        // not destroyed, and comes back with the app. Either way it must stop floating over other
        // applications: these are override-redirect windows, above everything, and no window manager will
        // put them away for us.
        const bool ours = _focusIsOurs();
        if (ours) m_sawAppFocus = true;
        if (m_sawAppFocus && !ours) {
            if (!m_active.empty()) { closeAll(); m_sawAppFocus = false; }
            for (auto& fn : m_floating)
                if (fn.win->window().isMapped()) { fn.win->window().setMapped(false); m_hiddenFloating = true; }
            return;                                   // nothing of ours is on screen to drive this frame
        }
        if (ours && m_hiddenFloating) {                // back in the app: the palettes return as they were
            for (auto& fn : m_floating) fn.win->window().setMapped(true);
            m_hiddenFloating = false;
        }
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
            else for (auto& p : m_active) if (p->isViewable()) { _auditPlacement(p.get()); JPrimitiveBuffer b; p->render(hal, b); }
        }

        if (!m_floating.empty()) {
            // Poll every floating popup (torn root or submenu child) for its own events. Hover fires the cascade
            // via hoverItem → hoverFloating, DEFERRED (m_isPolling), so the m_floating list is never mutated mid-
            // iteration; the deferred actions (open/close submenus) run afterwards, then closes are applied.
            m_isPolling = true;
            std::vector<JPopupWindow*> closing;
            for (auto& fn : m_floating) {
                if (!fn.win->window().isMapped()) continue;   // hidden with the app: no input, nothing to draw
                if (fn.win->pollFloating() == JPopupWindow::JFloatPollResult::Close) closing.push_back(fn.win.get());
            }
            m_isPolling = false;

            if (!m_deferred.empty()) {
                auto acts = std::move(m_deferred);
                m_deferred.clear();
                for (auto& a : acts) a();
            }
            for (auto* w : closing) closeFloatingSubtree(w, /*includeRoot=*/true);   // a closed menu takes its submenus with it

            for (auto& fn : m_floating)
                if (fn.win->window().isMapped() && fn.win->isViewable()) { JPrimitiveBuffer b; fn.win->render(hal, b); }
        }
    }

    void closeAll() {
        if (m_hal) for (auto& p : m_active) p->destroySurface(*m_hal);
        m_active.clear();
        m_placeAudited.clear();   // the pointers die with the popups; never compare against a freed one
        if (m_bar) m_bar->closeMenu();
    }

private:
    // "This axis has nothing to flip about" — the popup may only slide back inside the work area.
    static constexpr int kNoFlip = std::numeric_limits<int>::min();

    void openMenu(JMenu* menu, int sx, int sy, bool parentTorn, int flipX = kNoFlip, int flipY = kNoFlip) {
        if (!menu) { closeAll(); return; }
        if (m_active.empty()) m_sawAppFocus = false;   // a new cascade re-arms the leave-the-app check
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

        // Fit the room BEFORE placing it: a list too tall for the screen wraps into columns, so what
        // follows is placing a popup that can actually fit rather than clamping one that cannot.
        {
            const auto wa0 = popup->window().workAreaAt(sx, sy);
            popup->wrapToHeight(static_cast<uint32_t>(std::max(0, wa0.h)));
        }
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
            // A menu that ends up off the work area is a BUG, and one nobody can diagnose from a
            // screenshot: the same picture results from a mis-measured height, a wrong work area, or a
            // move that did not take. So say so unprompted, with all four inputs — at Warn, because it
            // means items are unreachable, and needing an env var set in advance to catch it means it is
            // only ever caught by someone who already suspected it.
            const bool off = (y + h > wa.y + wa.h) || (x + w > wa.x + wa.w) || y < wa.y || x < wa.x;
            JLOGC("menu.place", off ? JLogLevel::Warn : JLogLevel::Debug)
                << (off ? "OFF-SCREEN " : "") << "place sx=" << sx << " sy=" << sy
                << " w=" << w << " h=" << h << " wa=(" << wa.x << "," << wa.y << " " << wa.w << "x" << wa.h
                << ") flipY=" << flipY << " -> x=" << x << " y=" << y;
            if (x != sx || y != sy) popup->window().setPosition(x, y);
        }
        return popup;
    }

    // Is the popup actually on the screen, now that it IS on the screen?
    //
    // The check inside buildMenuPopup marks its own homework: it warns when the rectangle it just computed
    // falls outside the work area, which only ever catches arithmetic. It cannot catch the case where the
    // sum was right and the window is somewhere else — a size that grew after placement, a move that did
    // not take, a work area read before the desktop's panels were up. That case is INVISIBLE: the log
    // stays silent while the menu hangs off the bottom of the screen, so a report of "it still runs off"
    // has nothing to work from and the arithmetic reads as correct because it is.
    //
    // So ask the WINDOW where it ended up, once, on the first frame it is viewable, and warn on what is
    // actually there. Both sizes are logged because they answer different questions: the popup's own
    // (what placement used) against the window's (what the server has), which is the whole difference
    // between "we placed it wrong" and "it did not go where we put it".
    void _auditPlacement(JPopupWindow* p) {
        for (const void* seen : m_placeAudited) if (seen == p) return;
        m_placeAudited.push_back(p);
        const int x = p->window().screenX(), y = p->window().screenY();
        const int w = static_cast<int>(p->width()), h = static_cast<int>(p->height());
        const auto wa = p->window().workAreaAt(x, y);
        const bool off = x < wa.x || y < wa.y || x + w > wa.x + wa.w || y + h > wa.y + wa.h;
        JLOGC("menu.place", off ? JLogLevel::Warn : JLogLevel::Debug)
            << (off ? "OFF-SCREEN AS MAPPED " : "mapped ") << "at=(" << x << "," << y << ")"
            << " popup=" << w << "x" << h
            << " window=" << p->window().width() << "x" << p->window().height()
            << " wa=(" << wa.x << "," << wa.y << " " << wa.w << "x" << wa.h << ")"
            << (off ? "  <- the menu is off the work area where it is DRAWN" : "");
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
    bool m_sawAppFocus{false};    // this menu has held the app's focus at least once (see updateAndRender)
    bool m_hiddenFloating{false}; // torn-off menus we hid on leaving the app, to restore on return
    std::vector<std::unique_ptr<JPopupWindow>> m_active;     // modal dropdown stack
    std::vector<FloatNode>                     m_floating;   // torn-off menus + their submenu cascades
    std::vector<std::function<void()>>         m_deferred;   // run after polling
    // Popups whose mapped geometry has already been audited (see _auditPlacement) — once each, not per
    // frame. Raw pointers, only ever compared, and cleared with the stack that owns them.
    std::vector<const void*>                   m_placeAudited;
    bool                              m_isPolling{false};
};

} // inline namespace jf
