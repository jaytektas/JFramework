#pragma once

// ============================================================================
// jf::JShortcutRegistry — the app-global keyboard-shortcut desk.
//
// One place the runner asks "does any command claim this key event?". Register
// a JAction (indexed by its JKeySequence) so its shortcut is live, or register
// a bare JKeySequence + callback for a shortcut with no full command object.
// dispatch(ke) walks the registrations and, on the first ENABLED match, fires
// it and returns true (consumed) so the runner stops routing that key.
//
//   jShortcuts().registerAction(&saveAction);
//   jShortcuts().registerShortcut(JKeySequence::parse("Ctrl+Q"), []{ quit(); });
//   ...
//   if (jShortcuts().dispatch(keyEvent)) continue;   // in the key loop
//
// This complements MenuSystem.h's JMenuManager (menu-row accelerators): that
// stays for menu items; this is the shared registry actions/standalone chords
// use. A registered JAction that is disabled is silently skipped (its shortcut
// is dead while it's greyed) — matching the menu row's behaviour.
//
// Thread model: main-thread only, mirroring JMenuManager. Pointers registered
// here are NOT owned; unregister() (or unregisterAll on teardown) before an
// action dies.
// ============================================================================

#include "Action.h"
#include "KeySequence.h"
#include "KeyEvent.h"

#include <vector>
#include <functional>
#include <algorithm>

inline namespace jf {

class JShortcutRegistry {
public:
    // Register an action by its current shortcut. No-op if the action has no
    // valid shortcut. Re-registering the same action pointer refreshes it.
    void registerAction(JAction* action) {
        if (!action || !action->shortcut().valid()) return;
        // Drop any stale entry for this exact pointer first (its chord may have changed).
        m_actions.erase(std::remove_if(m_actions.begin(), m_actions.end(),
                            [action](const ActionReg& r) { return r.action == action; }),
                        m_actions.end());
        m_actions.push_back({action->shortcut(), action});
    }

    // Register a standalone chord + callback (no JAction). Ignored if invalid.
    // Returns an id that unregisterShortcut() can use to remove it later.
    uint32_t registerShortcut(const JKeySequence& seq, std::function<void()> callback) {
        if (!seq.valid() || !callback) return 0;
        const uint32_t id = ++m_nextId;
        m_shortcuts.push_back({id, seq, std::move(callback)});
        return id;
    }

    void unregisterAction(JAction* action) {
        m_actions.erase(std::remove_if(m_actions.begin(), m_actions.end(),
                            [action](const ActionReg& r) { return r.action == action; }),
                        m_actions.end());
    }

    void unregisterShortcut(uint32_t id) {
        m_shortcuts.erase(std::remove_if(m_shortcuts.begin(), m_shortcuts.end(),
                            [id](const StandaloneReg& r) { return r.id == id; }),
                          m_shortcuts.end());
    }

    void clear() { m_actions.clear(); m_shortcuts.clear(); }

    // Try to consume a key event. Returns true and fires the FIRST match:
    //   * a registered JAction whose (live) shortcut matches AND that is enabled
    //     — triggered via action->trigger() (so checkables toggle too);
    //   * else a standalone chord whose sequence matches.
    // A matching-but-disabled action does NOT consume — the key falls through.
    bool dispatch(const JKeyEvent& ke) {
        if (!ke.pressed) return false;
        for (const auto& r : m_actions) {
            if (!r.action) continue;
            // Match against the action's CURRENT shortcut (it may have been re-bound).
            if (!r.action->shortcut().matches(ke)) continue;
            if (!r.action->isEnabled()) continue;   // greyed command → dead shortcut, fall through
            r.action->trigger();
            return true;
        }
        for (const auto& r : m_shortcuts) {
            if (r.seq.matches(ke)) { r.callback(); return true; }
        }
        return false;
    }

    size_t actionCount()    const noexcept { return m_actions.size(); }
    size_t shortcutCount()  const noexcept { return m_shortcuts.size(); }

private:
    struct ActionReg {
        JKeySequence seq;       // snapshot at register time (dispatch re-reads live shortcut)
        JAction*     action{nullptr};
    };
    struct StandaloneReg {
        uint32_t              id{0};
        JKeySequence          seq;
        std::function<void()> callback;
    };

    std::vector<ActionReg>     m_actions;
    std::vector<StandaloneReg> m_shortcuts;
    uint32_t                   m_nextId{0};
};

// The app-global registry the runner dispatches through.
inline JShortcutRegistry& jShortcuts() {
    static JShortcutRegistry inst;
    return inst;
}

} // inline namespace jf
