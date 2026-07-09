#pragma once

// ============================================================================
// j/model/ItemDelegate.h — JAbstractItemDelegate (the "D" of model/view).
//
// A delegate owns the presentation and editing of a *single* cell, keeping that
// concern out of both the model (pure data) and the view (layout/scrolling).
// This header defines the abstract contract plus a concrete default that turns
// a cell's JVariant roles into displayable text — enough for a view to adopt
// without any renderer coupling. Actual painting is left to a view-side
// subclass that has a JPrimitiveBuffer; those overrides live with the view.
// ============================================================================

#include "j/model/ItemModel.h"
#include "j/core/Variant.h"

#include <string>

inline namespace jf {

// Geometry primitive kept local so the model layer stays render-stack-free.
struct JItemRect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    bool isEmpty() const { return w <= 0.f || h <= 0.f; }
};

// The per-cell inputs a delegate needs to render/measure one item. A view fills
// this in from its own geometry and hands it to the delegate.
struct JStyleOptionItem {
    JItemRect   rect;              // cell bounds in view coordinates
    bool        selected = false;  // part of the selection set
    bool        current  = false;  // the current (focused) index
    bool        enabled  = true;   // model flags say Enabled
    int         alignment = 0;     // opaque alignment hint (role Alignment)
};

// ----------------------------------------------------------------------------
// JAbstractItemDelegate — the contract. A view calls displayText()/sizeHint()
// while laying out, and (view-side subclasses) paint()/createEditor() while
// rendering/editing. Only the text/size hooks are needed by the pure model
// layer, so those are concrete here; paint is a no-op an owner overrides.
// ----------------------------------------------------------------------------
class JAbstractItemDelegate {
public:
    virtual ~JAbstractItemDelegate() = default;

    // Text a view should draw for this index (Display role by default).
    virtual std::string displayText(const JModelIndex& index) const {
        if (!index.isValid()) return {};
        return index.data(JItemRole::Display).toString();
    }

    // Preferred pixel size of the cell. Base gives a text-agnostic default; a
    // view/font-aware subclass refines it. `option` carries the available rect.
    virtual JItemRect sizeHint(const JStyleOptionItem& option,
                               const JModelIndex& index) const {
        (void)index;
        JItemRect r = option.rect;
        if (r.h <= 0.f) r.h = m_defaultRowHeight;
        return r;
    }

    // Paint the cell. The pure model layer has no renderer, so the base is a
    // no-op; a view supplies a subclass bound to its JPrimitiveBuffer.
    virtual void paint(const JStyleOptionItem& /*option*/,
                       const JModelIndex& /*index*/) const {}

    // Whether this delegate offers in-place editing for the index. Views query
    // this before spawning an editor.
    virtual bool isEditable(const JModelIndex& index) const {
        if (!index.isValid() || !index.model()) return false;
        return hasFlag(index.model()->flags(index), JItemFlag::Editable);
    }

    void  setDefaultRowHeight(float h) { m_defaultRowHeight = h; }
    float defaultRowHeight() const { return m_defaultRowHeight; }

protected:
    float m_defaultRowHeight = 22.0f;  // matches framework list-row default
};

// ----------------------------------------------------------------------------
// JStyledItemDelegate — the default concrete delegate. Formats a check state
// prefix when the item is Checkable and otherwise renders the Display text.
// Usable as-is by any view for basic text presentation.
// ----------------------------------------------------------------------------
class JStyledItemDelegate : public JAbstractItemDelegate {
public:
    std::string displayText(const JModelIndex& index) const override {
        if (!index.isValid()) return {};
        std::string text = index.data(JItemRole::Display).toString();
        if (index.model() && hasFlag(index.model()->flags(index), JItemFlag::Checkable)) {
            JVariant chk = index.data(JItemRole::Checked);
            std::string box = chk.toBool() ? "[x] " : "[ ] ";
            return box + text;
        }
        return text;
    }
};

}  // inline namespace jf
