#pragma once

// JUndoStack — a JFramework-native undo/redo command stack.
//
// A JCommand knows how to redo() and undo() one edit. JUndoStack::push() executes the command
// (via redo()) and records it; undo()/redo() walk the history. Pushing after some undos discards
// the redoable tail (the classic linear history). Consecutive same-id commands can coalesce
// (mergeWith) so a burst of edits — e.g. dragging a value — collapses into one undo step. A
// "clean" marker tracks the saved state for document-modified indicators. onChanged fires on any
// state change so a menu/toolbar can refresh its Undo/Redo enablement and labels.
//
// Thread-safety: MAIN THREAD ONLY (edits run on the UI thread).

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <utility>
#include <algorithm>

#include "Signal.h"

inline namespace jf {

// ----------------------------------------------------------------------------
// JCommand — one reversible edit. Subclass for stateful edits, or use JLambdaCommand.
// ----------------------------------------------------------------------------
class JCommand {
public:
    virtual ~JCommand() = default;

    // redo() is also the INITIAL apply — JUndoStack::push() calls it once when recording.
    virtual void redo() = 0;
    virtual void undo() = 0;

    // Human-readable label ("Move Panel", "Set Cell") for Undo/Redo menu text.
    virtual std::string text() const { return {}; }

    // Merge support: a command with id() >= 0 may absorb a following command of the SAME id
    // (return true after folding `next` into this one), collapsing a burst into one undo step.
    // id() < 0 (default) disables merging.
    virtual int  id() const { return -1; }
    virtual bool mergeWith(const JCommand& /*next*/) { return false; }
};

// ----------------------------------------------------------------------------
// JLambdaCommand — the common case: supply redo/undo closures, no subclass needed.
// ----------------------------------------------------------------------------
class JLambdaCommand : public JCommand {
public:
    JLambdaCommand(std::string text, std::function<void()> redo, std::function<void()> undo, int id = -1)
        : m_text(std::move(text)), m_redo(std::move(redo)), m_undo(std::move(undo)), m_id(id) {}

    void redo() override { if (m_redo) m_redo(); }
    void undo() override { if (m_undo) m_undo(); }
    std::string text() const override { return m_text; }
    int id() const override { return m_id; }

private:
    std::string           m_text;
    std::function<void()> m_redo, m_undo;
    int                   m_id;
};

// ----------------------------------------------------------------------------
// JUndoStack — the history.
// ----------------------------------------------------------------------------
class JUndoStack {
public:
    // Fires whenever canUndo/canRedo/index/clean/text may have changed.
    jf::JSignal<> onChanged;

    // Execute `cmd` (redo()) and record it. If the top command shares a non-negative id and
    // accepts the merge, `cmd` is folded into it instead of appended. Otherwise any redoable
    // tail is discarded and `cmd` becomes the new top. Honours the undo limit.
    void push(std::unique_ptr<JCommand> cmd) {
        if (!cmd) return;
        cmd->redo();

        const bool atTip = (m_index == static_cast<int>(m_cmds.size()));
        if (atTip && !m_cmds.empty() && cmd->id() >= 0 &&
            m_cmds.back()->id() == cmd->id() && m_cmds.back()->mergeWith(*cmd)) {
            emitChanged();                       // merged into the existing top
            return;
        }

        // Drop the redoable tail, then append.
        if (m_index < static_cast<int>(m_cmds.size()))
            m_cmds.erase(m_cmds.begin() + m_index, m_cmds.end());
        m_cmds.push_back(std::move(cmd));
        m_index = static_cast<int>(m_cmds.size());

        // Enforce the undo limit by dropping the oldest commands.
        if (m_limit > 0 && static_cast<int>(m_cmds.size()) > m_limit) {
            const int drop = static_cast<int>(m_cmds.size()) - m_limit;
            m_cmds.erase(m_cmds.begin(), m_cmds.begin() + drop);
            m_index -= drop;
            if (m_cleanIndex >= 0) m_cleanIndex -= drop;   // <0 = clean state fell off the stack
        }
        emitChanged();
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
        m_index = 0;
        m_cleanIndex = 0;
        emitChanged();
    }

    // 0 = unlimited. Setting a positive limit trims immediately.
    void setUndoLimit(int n) {
        m_limit = n;
        if (m_limit > 0 && static_cast<int>(m_cmds.size()) > m_limit) {
            const int drop = static_cast<int>(m_cmds.size()) - m_limit;
            m_cmds.erase(m_cmds.begin(), m_cmds.begin() + drop);
            m_index = std::max(0, m_index - drop);
            if (m_cleanIndex >= 0) m_cleanIndex -= drop;
            emitChanged();
        }
    }
    int undoLimit() const { return m_limit; }

    // Clean-state tracking for a "document modified" flag.
    void setClean()      { m_cleanIndex = m_index; emitChanged(); }
    bool isClean() const { return m_index == m_cleanIndex; }

private:
    void emitChanged() { onChanged.emit(); }

    std::vector<std::unique_ptr<JCommand>> m_cmds;
    int m_index{0};        // count of applied commands (history head sits just past index-1)
    int m_cleanIndex{0};   // index that equals the saved state (<0 = saved state trimmed away)
    int m_limit{0};        // 0 = unlimited
};

// ----------------------------------------------------------------------------
// JUndoGroup — several stacks (e.g. one per open document/surface) with one active. A single
// pair of Undo/Redo actions can drive whichever stack is active.
// ----------------------------------------------------------------------------
class JUndoGroup {
public:
    jf::JSignal<> onChanged;   // active stack changed

    void add(JUndoStack* s)          { if (s) m_stacks.push_back(s); }
    void setActive(JUndoStack* s)    { m_active = s; onChanged.emit(); }
    JUndoStack* active() const       { return m_active; }

    void undo() { if (m_active) m_active->undo(); }
    void redo() { if (m_active) m_active->redo(); }
    bool canUndo() const { return m_active && m_active->canUndo(); }
    bool canRedo() const { return m_active && m_active->canRedo(); }

private:
    std::vector<JUndoStack*> m_stacks;   // non-owning
    JUndoStack*              m_active{nullptr};
};

} // inline namespace jf
