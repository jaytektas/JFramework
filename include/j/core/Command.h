#pragma once

// ============================================================================
// JUndoCommand — one reversible edit in the JFramework undo/redo model.
//
// A command knows how to apply an edit (redo(), which is ALSO the initial apply)
// and reverse it (undo()). It carries a human-readable label (text()) for an
// Undo/Redo menu entry, and an optional merge protocol (id() + mergeWith()) so a
// burst of same-kind edits — consecutive typing, a value drag — can collapse into
// a single undo step instead of flooding the history.
//
// This header is pure data-structure: no rendering, no widgets, no GPU. Subclass
// JUndoCommand for stateful edits, or reach for JFunctionCommand for the common
// closure-based case.
//
// Thread-safety: MAIN THREAD ONLY — edits run on the UI thread.
// ============================================================================

#include <string>
#include <functional>
#include <utility>

inline namespace jf {

// ----------------------------------------------------------------------------
// JUndoCommand — abstract base. redo() is invoked once by JUndoStack::push() to
// perform the initial apply, then again on every subsequent redo().
// ----------------------------------------------------------------------------
class JUndoCommand {
public:
    virtual ~JUndoCommand() = default;

    // Apply the edit. Called once by push() as the initial apply, and on redo().
    virtual void redo() = 0;
    // Reverse the edit applied by redo().
    virtual void undo() = 0;

    // Label for the Undo/Redo menu ("Move Panel", "Type", ...). Empty by default.
    virtual std::string text() const { return {}; }

    // Merge protocol. A command with id() >= 0 may absorb a following command of
    // the SAME id: JUndoStack::push() offers the incoming command to the current
    // top via mergeWith(); returning true folds it in (this command must update
    // its own new-state so a single undo reverts the whole burst) and the incoming
    // command is dropped. id() < 0 (the default) disables merging entirely.
    virtual int  id() const { return -1; }
    virtual bool mergeWith(const JUndoCommand* /*next*/) { return false; }
};

// ----------------------------------------------------------------------------
// JFunctionCommand — the common case: supply a label plus redo/undo closures, no
// subclass required. Pass a non-negative id to opt into merging; the default
// mergeWith() keeps the older command's undo closure and adopts the newer
// command's redo closure, which is the right behaviour for a value that is being
// dragged/retyped through a sequence of intermediate states.
// ----------------------------------------------------------------------------
class JFunctionCommand : public JUndoCommand {
public:
    JFunctionCommand(std::string label,
                     std::function<void()> redoFn,
                     std::function<void()> undoFn,
                     int mergeId = -1)
        : m_text(std::move(label))
        , m_redo(std::move(redoFn))
        , m_undo(std::move(undoFn))
        , m_id(mergeId) {}

    void        redo() override       { if (m_redo) m_redo(); }
    void        undo() override       { if (m_undo) m_undo(); }
    std::string text() const override { return m_text; }
    int         id()   const override { return m_id; }

    // Fold a following same-id command into this one: adopt its "redo" so the
    // merged command replays straight to the newest state, keep our "undo" so a
    // single undo returns to the state before the burst began.
    bool mergeWith(const JUndoCommand* next) override {
        if (m_id < 0 || !next || next->id() != m_id) return false;
        const auto* other = static_cast<const JFunctionCommand*>(next);
        m_redo = other->m_redo;
        m_text = other->m_text;
        return true;
    }

private:
    std::string           m_text;
    std::function<void()> m_redo;
    std::function<void()> m_undo;
    int                   m_id;
};

} // inline namespace jf
