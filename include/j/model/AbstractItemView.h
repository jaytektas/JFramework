#pragma once

// ============================================================================
// j/model/AbstractItemView.h — JAbstractItemView (the "V" contract).
//
// The thin, render-agnostic interface a concrete widget adopts to become a
// model/view view. It owns *no* pixels and does *no* drawing — it only wires a
// widget to a JAbstractItemModel and a JItemSelectionModel, reacts to their
// signals, and exposes the two spatial hooks every view must answer:
//
//   indexAt(x, y)   — hit-test a point to the item under it
//   visualRect(idx) — where an item is drawn, for scroll-to / editor placement
//
// A widget (e.g. a future JListView rework) multiply-inherits this alongside
// its JControl base and overrides the two hooks; this header deliberately does
// NOT touch any existing widget so integration stays a separate step.
// ============================================================================

#include "j/model/ItemModel.h"
#include "j/model/SelectionModel.h"
#include "j/model/ItemDelegate.h"
#include "j/core/Signal.h"

#include <memory>

inline namespace jf {

class JAbstractItemView : public jf::JSlotTracker {
public:
    ~JAbstractItemView() override = default;

    // ---- Model -------------------------------------------------------------
    virtual void setModel(JAbstractItemModel* model) {
        if (m_model == model) return;
        m_modelConns.disconnectAll();
        m_model = model;

        // Ensure a selection model that targets the same data source.
        if (!m_ownedSelection && !m_selection) {
            m_ownedSelection = std::make_unique<JItemSelectionModel>(model);
            m_selection = m_ownedSelection.get();
        } else if (m_selection) {
            m_selection->setModel(model);
        }

        if (m_model) {
            m_modelConns.addConnection(m_model->dataChanged.connect(
                [this](JModelIndex tl, JModelIndex br) { dataChangedEvent(tl, br); }));
            m_modelConns.addConnection(m_model->rowsInserted.connect(
                [this](JModelIndex p, int f, int l) { rowsInsertedEvent(p, f, l); }));
            m_modelConns.addConnection(m_model->rowsRemoved.connect(
                [this](JModelIndex p, int f, int l) { rowsRemovedEvent(p, f, l); }));
            m_modelConns.addConnection(m_model->modelReset.connect(
                [this]() { modelResetEvent(); }));
        }
        onModelChanged.emit();
        reset();
    }
    JAbstractItemModel* model() const { return m_model; }

    // ---- Selection ---------------------------------------------------------
    virtual void setSelectionModel(JItemSelectionModel* selection) {
        if (m_selection == selection) return;
        m_selectionConns.disconnectAll();
        m_ownedSelection.reset();
        m_selection = selection;
        if (m_selection) {
            if (m_model) m_selection->setModel(m_model);
            m_selectionConns.addConnection(m_selection->currentChanged.connect(
                [this](JModelIndex c, JModelIndex p) { currentChangedEvent(c, p); }));
            m_selectionConns.addConnection(m_selection->selectionChanged.connect(
                [this](std::vector<JModelIndex> s, std::vector<JModelIndex> d) {
                    selectionChangedEvent(s, d);
                }));
        }
    }
    JItemSelectionModel* selectionModel() const { return m_selection; }

    // ---- Delegate ----------------------------------------------------------
    void setItemDelegate(JAbstractItemDelegate* d) { m_delegate = d; }
    JAbstractItemDelegate* itemDelegate() const {
        return m_delegate ? m_delegate : &defaultDelegate();
    }

    // ---- Spatial contract (implemented by the concrete widget) ------------
    // Item at a view-local point, or an invalid index if none.
    virtual JModelIndex indexAt(float x, float y) const = 0;
    // Where an index is drawn (empty rect if off-screen / invalid).
    virtual JItemRect  visualRect(const JModelIndex& index) const = 0;
    // Bring an index into view (no-op default).
    virtual void scrollTo(const JModelIndex& /*index*/) {}

    // Convenience: current index passthrough.
    JModelIndex currentIndex() const {
        return m_selection ? m_selection->currentIndex() : JModelIndex();
    }
    void setCurrentIndex(const JModelIndex& index) {
        if (m_selection)
            m_selection->setCurrentIndex(index,
                JItemSelectionModel::ClearAndSelect | JItemSelectionModel::Current);
    }

    // Emitted after the view swaps to a new model.
    jf::JSignal<> onModelChanged;

protected:
    // React hooks — concrete views override to re-layout / repaint. Defaults
    // fall back to a full reset so a minimal view still stays correct.
    virtual void dataChangedEvent(const JModelIndex&, const JModelIndex&) { reset(); }
    virtual void rowsInsertedEvent(const JModelIndex&, int, int) { reset(); }
    virtual void rowsRemovedEvent(const JModelIndex&, int, int) { reset(); }
    virtual void modelResetEvent() { reset(); }
    virtual void currentChangedEvent(const JModelIndex&, const JModelIndex&) {}
    virtual void selectionChangedEvent(const std::vector<JModelIndex>&,
                                       const std::vector<JModelIndex>&) {}
    // Recompute whatever the view caches; base does nothing.
    virtual void reset() {}

    static JStyledItemDelegate& defaultDelegate() {
        static JStyledItemDelegate d;
        return d;
    }

    JAbstractItemModel*     m_model     = nullptr;
    JItemSelectionModel*    m_selection = nullptr;
    JAbstractItemDelegate*  m_delegate  = nullptr;
    std::unique_ptr<JItemSelectionModel> m_ownedSelection;

    jf::JSlotTracker m_modelConns;
    jf::JSlotTracker m_selectionConns;
};

}  // inline namespace jf
