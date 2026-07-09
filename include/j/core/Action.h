#pragma once

// ============================================================================
// jf::JAction — a shared command object.
//
// One JAction is the single command a menu item, a toolbar button, AND a
// keyboard shortcut all point at: it holds the command's text/icon/tooltip,
// its enabled/visible/checkable state, and its JKeySequence shortcut, and it
// emits framework JSignals when it fires or its presentation changes. Wire the
// UI once to triggered()/toggled()/changed() and every surface that shows the
// action stays in lock-step — set enabled(false) once and the menu row greys,
// the toolbar button greys, and the shortcut stops firing.
//
//   JAction save("Save");
//   save.setShortcut(JKeySequence::parse("Ctrl+S"));
//   save.triggered.connect([]{ doSave(); });
//   jShortcuts().registerAction(&save);       // shortcut now live
//   ...
//   saveButton.onClicked.connect([&]{ save.trigger(); });  // button routes here
//
// JAction is a JSlotTracker so callbacks it connects to auto-disconnect, and so
// it can be a signal receiver. Non-copyable (a command has one identity).
// ============================================================================

#include "Signal.h"
#include "KeySequence.h"

#include <string>

inline namespace jf {

class JAction : public JSlotTracker {
public:
    // Fired by trigger() — "the command ran". For a checkable action, toggled()
    // fires alongside with the new checked state.
    jf::JSignal<>     triggered;
    jf::JSignal<bool> toggled;
    // Fired whenever any presentation/state property changes (text, icon,
    // enabled, visible, checked, shortcut, tips) so views can refresh.
    jf::JSignal<>     changed;

    JAction() = default;
    explicit JAction(std::string text) : m_text(std::move(text)) {}
    JAction(std::string text, JKeySequence shortcut)
        : m_text(std::move(text)), m_shortcut(shortcut) {}

    JAction(const JAction&) = delete;
    JAction& operator=(const JAction&) = delete;

    // --- presentation -------------------------------------------------------
    const std::string& text() const noexcept { return m_text; }
    void setText(std::string t) { if (t != m_text) { m_text = std::move(t); changed.emit(); } }

    // Icon is an opaque id/string (an atlas key or a resource name) — the view
    // decides how to resolve it. Empty = no icon.
    const std::string& icon() const noexcept { return m_icon; }
    void setIcon(std::string id) { if (id != m_icon) { m_icon = std::move(id); changed.emit(); } }

    const std::string& tooltip() const noexcept { return m_tooltip; }
    void setTooltip(std::string t) { if (t != m_tooltip) { m_tooltip = std::move(t); changed.emit(); } }

    const std::string& statusTip() const noexcept { return m_statusTip; }
    void setStatusTip(std::string t) { if (t != m_statusTip) { m_statusTip = std::move(t); changed.emit(); } }

    // --- state --------------------------------------------------------------
    bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool e) { if (e != m_enabled) { m_enabled = e; changed.emit(); } }

    bool isVisible() const noexcept { return m_visible; }
    void setVisible(bool v) { if (v != m_visible) { m_visible = v; changed.emit(); } }

    bool isCheckable() const noexcept { return m_checkable; }
    void setCheckable(bool c) { if (c != m_checkable) { m_checkable = c; changed.emit(); } }

    bool isChecked() const noexcept { return m_checked; }
    // Set checked state directly (no trigger); emits toggled()+changed() on a
    // real change so views stay in sync.
    void setChecked(bool c) {
        if (!m_checkable || c == m_checked) return;
        m_checked = c;
        toggled.emit(m_checked);
        changed.emit();
    }

    // --- shortcut -----------------------------------------------------------
    const JKeySequence& shortcut() const noexcept { return m_shortcut; }
    void setShortcut(JKeySequence sc) { if (sc != m_shortcut) { m_shortcut = sc; changed.emit(); } }

    // --- activation ---------------------------------------------------------
    // Run the command: a disabled action is inert. A checkable action flips its
    // checked state (emitting toggled) first, then triggered() fires.
    void trigger() {
        if (!m_enabled) return;
        if (m_checkable) {
            m_checked = !m_checked;
            toggled.emit(m_checked);
            changed.emit();
        }
        triggered.emit();
    }

private:
    std::string  m_text;
    std::string  m_icon;
    std::string  m_tooltip;
    std::string  m_statusTip;
    JKeySequence m_shortcut;
    bool         m_enabled{true};
    bool         m_visible{true};
    bool         m_checkable{false};
    bool         m_checked{false};
};

} // inline namespace jf
