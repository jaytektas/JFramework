#pragma once

// ============================================================================
// j/model/SelectionModel.h — JItemSelectionModel.
//
// Selection state as a first-class, shareable object, decoupled from any view.
// Multiple views can share one selection model over one JAbstractItemModel, and
// they all update together through the currentChanged / selectionChanged
// signals. Holds two independent notions:
//
//   * current index  — the "cursor"/keyboard focus item (one, or invalid)
//   * selection set   — the set of highlighted items (zero or more)
//
// Selection behaviour is governed by a SelectionMode (Single / Multi /
// Extended) and per-call SelectFlags, so the same model serves a radio-style
// single-pick list and a shift/ctrl multi-select grid.
// ============================================================================

#include "j/model/ItemModel.h"
#include "j/core/Signal.h"

#include <vector>
#include <algorithm>

inline namespace jf {

class JItemSelectionModel {
public:
    // How a plain click / select() behaves by default.
    enum class SelectionMode { Single, Multi, Extended };

    // Per-call intent. Combine with |. Clear wipes the current set first;
    // Toggle flips membership; ClearAndSelect is the common "replace" case.
    enum SelectFlag : uint32_t {
        NoUpdate      = 0,
        Clear         = 1u << 0,
        Select        = 1u << 1,
        Deselect      = 1u << 2,
        Toggle        = 1u << 3,
        Current       = 1u << 4,   // also make the index current
        ClearAndSelect = Clear | Select,
    };

    explicit JItemSelectionModel(JAbstractItemModel* model = nullptr,
                                 SelectionMode mode = SelectionMode::Extended)
        : m_model(model), m_mode(mode) {}

    JAbstractItemModel* model() const { return m_model; }
    void setModel(JAbstractItemModel* model) {
        if (m_model == model) return;
        m_model = model;
        clear();
    }

    SelectionMode selectionMode() const { return m_mode; }
    void setSelectionMode(SelectionMode m) { m_mode = m; }

    // ---- Current index -----------------------------------------------------
    const JModelIndex& currentIndex() const { return m_current; }

    void setCurrentIndex(const JModelIndex& index, uint32_t flags = Select) {
        JModelIndex previous = m_current;
        if (!(index == previous)) {
            m_current = index;
            currentChanged.emit(m_current, previous);
        }
        if (flags != NoUpdate)
            select(index, flags);
    }

    // ---- Selection set -----------------------------------------------------
    bool isSelected(const JModelIndex& index) const {
        if (!index.isValid()) return false;
        return std::find(m_selected.begin(), m_selected.end(), index) != m_selected.end();
    }

    const std::vector<JModelIndex>& selectedIndexes() const { return m_selected; }
    bool hasSelection() const { return !m_selected.empty(); }

    // Apply a selection command to a single index, honouring SelectionMode.
    void select(const JModelIndex& index, uint32_t flags) {
        std::vector<JModelIndex> selectedNow, deselectedNow;

        bool effToggle   = (flags & Toggle) != 0;
        bool effSelect   = (flags & Select) != 0;
        bool effDeselect = (flags & Deselect) != 0;
        bool effClear    = (flags & Clear) != 0;

        // Single mode: any positive selection implies replacing the set.
        if (m_mode == SelectionMode::Single && (effSelect || effToggle))
            effClear = true;

        if (effClear)
            _clearInto(deselectedNow);

        if (index.isValid()) {
            bool present = isSelected(index);
            if (effToggle) {
                if (present) { _remove(index); deselectedNow.push_back(index); }
                else         { m_selected.push_back(index); selectedNow.push_back(index); }
            } else if (effDeselect) {
                if (present) { _remove(index); deselectedNow.push_back(index); }
            } else if (effSelect) {
                if (!present) { m_selected.push_back(index); selectedNow.push_back(index); }
            }
        }

        if (flags & Current) {
            JModelIndex prev = m_current;
            if (!(index == prev)) { m_current = index; currentChanged.emit(m_current, prev); }
        }

        if (!selectedNow.empty() || !deselectedNow.empty())
            selectionChanged.emit(selectedNow, deselectedNow);
    }

    // Select a contiguous span [first, last] of rows in one column (row-select
    // convenience for list/table extended selection).
    void selectRange(int firstRow, int lastRow, int column = 0,
                     uint32_t flags = ClearAndSelect) {
        if (!m_model) return;
        if (firstRow > lastRow) std::swap(firstRow, lastRow);
        std::vector<JModelIndex> selectedNow, deselectedNow;
        if (flags & Clear) _clearInto(deselectedNow);
        for (int r = firstRow; r <= lastRow; ++r) {
            JModelIndex idx = m_model->index(r, column);
            if (idx.isValid() && !isSelected(idx)) {
                m_selected.push_back(idx);
                selectedNow.push_back(idx);
            }
        }
        if (!selectedNow.empty() || !deselectedNow.empty())
            selectionChanged.emit(selectedNow, deselectedNow);
    }

    void clearSelection() {
        std::vector<JModelIndex> deselectedNow;
        _clearInto(deselectedNow);
        if (!deselectedNow.empty())
            selectionChanged.emit(std::vector<JModelIndex>{}, deselectedNow);
    }

    // Clear both current and selection.
    void clear() {
        clearSelection();
        JModelIndex prev = m_current;
        if (prev.isValid()) {
            m_current = JModelIndex();
            currentChanged.emit(m_current, prev);
        }
    }

    // ---- Signals -----------------------------------------------------------
    // currentChanged(current, previous)
    jf::JSignal<JModelIndex, JModelIndex> currentChanged;
    // selectionChanged(newlySelected, newlyDeselected)
    jf::JSignal<std::vector<JModelIndex>, std::vector<JModelIndex>> selectionChanged;

private:
    void _remove(const JModelIndex& index) {
        m_selected.erase(std::remove(m_selected.begin(), m_selected.end(), index),
                         m_selected.end());
    }
    void _clearInto(std::vector<JModelIndex>& deselectedOut) {
        for (auto& idx : m_selected) deselectedOut.push_back(idx);
        m_selected.clear();
    }

    JAbstractItemModel*      m_model = nullptr;
    SelectionMode            m_mode  = SelectionMode::Extended;
    JModelIndex              m_current;
    std::vector<JModelIndex> m_selected;
};

}  // inline namespace jf
