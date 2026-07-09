#pragma once

// ============================================================================
// JUndoStack — a JFramework-native linear undo/redo history.
//
// push(cmd) executes the command (via redo()) and records it. undo()/redo() walk
// the history; pushing after some undos discards the redoable tail (classic
// linear history). Consecutive same-id commands can coalesce (mergeWith) so a
// burst of edits collapses into one undo step. beginMacro()/endMacro() group
// several pushes into one compound, undoable-as-one step (nestable). A "clean"
// marker tracks the last-saved index for a document-modified indicator.
//
// State changes are reported through typed jf::JSignals — indexChanged,
// canUndoChanged, canRedoChanged, cleanChanged — each fired ONLY on an actual
// transition, so menus/toolbars/title bars can bind directly without debouncing.
//
// Thread-safety: MAIN THREAD ONLY (edits run on the UI thread).
// ============================================================================

#include <vector>
#include <memory>
#include <string>
#include <utility>
#include <algorithm>

#include "Signal.h"
#include "Command.h"

inline namespace jf {

// ----------------------------------------------------------------------------
// JMacroCommand — a compound command: a group of child commands applied forward
// on redo() and reversed on undo(). Built by JUndoStack::beginMacro/endMacro.
// ----------------------------------------------------------------------------
class JMacroCommand : public JUndoCommand {
public:
    explicit JMacroCommand(std::string text) : m_text(std::move(text)) {}

    void add(std::unique_ptr<JUndoCommand> child) { m_children.push_back(std::move(child)); }
    bool empty() const { return m_children.empty(); }

    void redo() override {
        for (auto& c : m_children) c->redo();
    }
    void undo() override {
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) (*it)->undo();
    }
    std::string text() const override { return m_text; }

private:
    std::string                                 m_text;
    std::vector<std::unique_ptr<JUndoCommand>>  m_children;
};

// ----------------------------------------------------------------------------
// JUndoStack — the history.
// ----------------------------------------------------------------------------
class JUndoStack {
public:
    // Fired ONLY on an actual transition of the relevant state.
    jf::JSignal<int>  indexChanged;    // new applied-command count (history head)
    jf::JSignal<bool> canUndoChanged;
    jf::JSignal<bool> canRedoChanged;
    jf::JSignal<bool> cleanChanged;

    // Execute `cmd` (redo()) and record it, TAKING OWNERSHIP of the raw pointer.
    // Inside a macro, the command is applied and folded into the open macro instead
    // of touching the visible history. At the tip, a non-negative id that matches
    // the top command and is accepted by mergeWith() is coalesced. Otherwise any
    // redoable tail is discarded, the command becomes the new top, and the undo
    // limit is enforced.
    void push(JUndoCommand* raw) {
        std::unique_ptr<JUndoCommand> cmd(raw);
        if (!cmd) return;

        cmd->redo();   // initial apply

        if (!m_macroStack.empty()) {
            m_macroStack.back()->add(std::move(cmd));   // fold into open macro; no visible change
            return;
        }

        const bool atTip = (m_index == static_cast<int>(m_cmds.size()));
        if (atTip && !m_cmds.empty() && cmd->id() >= 0 &&
            m_cmds.back()->id() == cmd->id() && m_cmds.back()->mergeWith(cmd.get())) {
            emitChanged();   // merged into the existing top (index unchanged, text may change)
            return;
        }

        recordCommitted(std::move(cmd));   // children/self already applied — do not re-run redo()
    }

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index < static_cast<int>(m_cmds.size()); }

    void undo() { if (canUndo()) { m_cmds[--m_index]->undo(); emitChanged(); } }
    void redo() { if (canRedo()) { m_cmds[m_index++]->redo(); emitChanged(); } }

    std::string undoText() const { return canUndo() ? m_cmds[m_index - 1]->text() : std::string{}; }
    std::string redoText() const { return canRedo() ? m_cmds[m_index]->text()     : std::string{}; }

    int  count() const { return static_cast<int>(m_cmds.size()); }
    int  index() const { return m_index; }

    void clear() {
        m_cmds.clear();
        m_macroStack.clear();
        m_index = 0;
        m_cleanIndex = 0;
        emitChanged();
    }

    // 0 = unlimited. Setting a positive limit trims the oldest immediately.
    void setUndoLimit(int n) {
        m_limit = n;
        if (m_limit > 0 && static_cast<int>(m_cmds.size()) > m_limit) {
            trimToLimit();
            emitChanged();
        }
    }
    int undoLimit() const { return m_limit; }

    // Compound edits: everything pushed between beginMacro and the matching
    // endMacro undoes/redoes as ONE step. Nestable — an inner macro becomes a
    // child of the enclosing one. An empty macro is discarded on endMacro.
    void beginMacro(std::string text) {
        m_macroStack.push_back(std::make_unique<JMacroCommand>(std::move(text)));
    }
    void endMacro() {
        if (m_macroStack.empty()) return;
        auto macro = std::move(m_macroStack.back());
        m_macroStack.pop_back();
        if (macro->empty()) return;                     // nothing recorded — drop it
        if (!m_macroStack.empty()) {
            m_macroStack.back()->add(std::move(macro));  // nest into the parent (already applied)
            return;
        }
        recordCommitted(std::move(macro));               // commit as one step (already applied)
    }
    bool inMacro() const { return !m_macroStack.empty(); }

    // Clean-state tracking for a "document modified" flag.
    void setClean() { m_cleanIndex = m_index; emitChanged(); }
    bool isClean() const { return m_index == m_cleanIndex; }

private:
    // Append an already-applied command as the new tip, dropping the redoable tail
    // and honouring the undo limit. Does NOT call redo().
    void recordCommitted(std::unique_ptr<JUndoCommand> cmd) {
        if (m_index < static_cast<int>(m_cmds.size()))
            m_cmds.erase(m_cmds.begin() + m_index, m_cmds.end());
        m_cmds.push_back(std::move(cmd));
        m_index = static_cast<int>(m_cmds.size());
        if (m_limit > 0 && static_cast<int>(m_cmds.size()) > m_limit)
            trimToLimit();
        emitChanged();
    }

    // Drop the oldest commands until the stack fits the (positive) limit.
    void trimToLimit() {
        const int drop = static_cast<int>(m_cmds.size()) - m_limit;
        if (drop <= 0) return;
        m_cmds.erase(m_cmds.begin(), m_cmds.begin() + drop);
        m_index = std::max(0, m_index - drop);
        if (m_cleanIndex >= 0) m_cleanIndex -= drop;   // <0 = saved state fell off the stack
    }

    // Emit only the typed signals whose state actually changed.
    void emitChanged() {
        if (m_index != m_lastIndex) { m_lastIndex = m_index; indexChanged.emit(m_index); }
        const bool cu = canUndo(); if (cu != m_lastCanUndo) { m_lastCanUndo = cu; canUndoChanged.emit(cu); }
        const bool cr = canRedo(); if (cr != m_lastCanRedo) { m_lastCanRedo = cr; canRedoChanged.emit(cr); }
        const bool cl = isClean(); if (cl != m_lastClean)   { m_lastClean   = cl; cleanChanged.emit(cl); }
    }

    std::vector<std::unique_ptr<JUndoCommand>>  m_cmds;
    std::vector<std::unique_ptr<JMacroCommand>> m_macroStack;   // open (possibly nested) macros
    int  m_index{0};        // count of applied commands (history head sits just past index-1)
    int  m_cleanIndex{0};   // index that equals the saved state (<0 = saved state trimmed away)
    int  m_limit{0};        // 0 = unlimited

    // Last-emitted snapshots so each signal fires only on a real transition.
    int  m_lastIndex{0};
    bool m_lastCanUndo{false};
    bool m_lastCanRedo{false};
    bool m_lastClean{true};
};

// ----------------------------------------------------------------------------
// JUndoGroup — several stacks (e.g. one per open document/surface) with one
// active. A single pair of Undo/Redo actions can drive whichever stack is active.
// Non-owning: the caller keeps the JUndoStacks alive.
// ----------------------------------------------------------------------------
class JUndoGroup {
public:
    jf::JSignal<JUndoStack*> activeChanged;

    void add(JUndoStack* s) { if (s) m_stacks.push_back(s); }
    void setActive(JUndoStack* s) { m_active = s; activeChanged.emit(s); }
    JUndoStack* active() const { return m_active; }

    void undo() { if (m_active) m_active->undo(); }
    void redo() { if (m_active) m_active->redo(); }
    bool canUndo() const { return m_active && m_active->canUndo(); }
    bool canRedo() const { return m_active && m_active->canRedo(); }

private:
    std::vector<JUndoStack*> m_stacks;   // non-owning
    JUndoStack*              m_active{nullptr};
};

} // inline namespace jf
