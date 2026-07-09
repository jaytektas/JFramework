#pragma once

// ============================================================================
// j/model/ItemModel.h — JFramework model layer: JModelIndex + JAbstractItemModel.
//
// A clean-room, original model/view foundation in the JFramework idiom. This is
// the "M" of model/view/delegate: an abstract, view-agnostic data source that
// speaks JVariant across a small set of roles and announces mutation through
// JSignal. Views (the "V") observe these signals and never own the data; a
// delegate (the "D", see ItemDelegate.h) renders/edits a single cell.
//
//   JStringListModel names({"crank", "cam", "map"});
//   names.rowCount();                       // 3
//   names.data(names.index(1), JItemRole::Display).toString();   // "cam"
//   names.dataChanged.connect([](auto tl, auto br){ ... });      // react
//
// This header is intentionally leaf: it depends only on JVariant and JSignal so
// the model layer can be unit-tested without pulling the widget/render stack.
// ============================================================================

#include "j/core/Variant.h"
#include "j/core/Signal.h"

#include <vector>
#include <string>
#include <cstdint>

inline namespace jf {

class JAbstractItemModel;   // forward

// ----------------------------------------------------------------------------
// Roles — the "aspect" of an item being requested. Mirrors the intent of a
// classic item role set but stays framework-native. User roles start at User.
// ----------------------------------------------------------------------------
enum class JItemRole : int {
    Display    = 0,   // primary text shown to the user
    Edit       = 1,   // value fed into / read back from an editor
    Tooltip    = 2,   // hover help
    Checked    = 3,   // check state (Bool JVariant)
    Decoration = 4,   // icon / colour swatch (custom JVariant payload)
    StatusTip  = 5,   // status-bar help
    Alignment  = 6,   // rendering alignment hint (Int)
    Foreground = 7,   // text colour hint (custom)
    Background = 8,   // fill colour hint (custom)
    UserData   = 9,   // opaque per-item payload not shown
    User       = 256  // first role free for application use
};
inline constexpr int roleId(JItemRole r) { return static_cast<int>(r); }

// ----------------------------------------------------------------------------
// Item flags — per-cell capability bitset.
// ----------------------------------------------------------------------------
enum class JItemFlag : uint32_t {
    None            = 0,
    Selectable      = 1u << 0,
    Editable        = 1u << 1,
    Enabled         = 1u << 2,
    Checkable       = 1u << 3,
    DragEnabled     = 1u << 4,
    DropEnabled     = 1u << 5,
    UserTristate    = 1u << 6,
};
inline constexpr JItemFlag operator|(JItemFlag a, JItemFlag b) {
    return static_cast<JItemFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr JItemFlag operator&(JItemFlag a, JItemFlag b) {
    return static_cast<JItemFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline constexpr JItemFlag& operator|=(JItemFlag& a, JItemFlag b) { a = a | b; return a; }
inline constexpr bool hasFlag(JItemFlag set, JItemFlag f) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(f)) != 0u;
}

// Orientation for header queries.
enum class JOrientation2 : uint8_t { Horizontal, Vertical };

// ----------------------------------------------------------------------------
// JModelIndex — a lightweight, copyable locator into a model. It is only a
// coordinate (row, column, model, opaque id); it never owns data and is only
// valid for as long as the model's structure is unchanged. An invalid index
// (default-constructed) denotes the conceptual root / "no item".
// ----------------------------------------------------------------------------
class JModelIndex {
public:
    JModelIndex() = default;

    int row()    const noexcept { return m_row; }
    int column() const noexcept { return m_col; }

    // Opaque per-node id/pointer a model may stash to relocate the item on the
    // next call (used chiefly by tree models). Two accessors for either idiom.
    void*    internalPointer() const noexcept { return m_ptr; }
    uint64_t internalId()      const noexcept { return m_id; }

    const JAbstractItemModel* model() const noexcept { return m_model; }

    bool isValid() const noexcept {
        return m_row >= 0 && m_col >= 0 && m_model != nullptr;
    }

    // Parent of this index, resolved through the owning model. Defined out of
    // line below (needs the full model type).
    JModelIndex parent() const;

    // Sibling in the same parent — convenience for view traversal.
    JModelIndex sibling(int row, int column) const;

    // Value at a role for this index (convenience forwarding to the model).
    JVariant data(int role = roleId(JItemRole::Display)) const;
    JVariant data(JItemRole role) const { return data(roleId(role)); }

    bool operator==(const JModelIndex& o) const noexcept {
        return m_row == o.m_row && m_col == o.m_col &&
               m_ptr == o.m_ptr && m_id == o.m_id && m_model == o.m_model;
    }
    bool operator!=(const JModelIndex& o) const noexcept { return !(*this == o); }

    // Deterministic ordering so indexes can live in ordered containers.
    bool operator<(const JModelIndex& o) const noexcept {
        if (m_row != o.m_row) return m_row < o.m_row;
        if (m_col != o.m_col) return m_col < o.m_col;
        return m_id < o.m_id;
    }

private:
    friend class JAbstractItemModel;
    JModelIndex(int row, int col, void* ptr, uint64_t id, const JAbstractItemModel* m)
        : m_row(row), m_col(col), m_ptr(ptr), m_id(id), m_model(m) {}

    int      m_row   = -1;
    int      m_col   = -1;
    void*    m_ptr   = nullptr;
    uint64_t m_id    = 0;
    const JAbstractItemModel* m_model = nullptr;
};

// ----------------------------------------------------------------------------
// JAbstractItemModel — abstract data source. Subclasses implement the four
// structural queries (rowCount / columnCount / index / parent) plus data();
// everything else has a sensible default. Mutation is announced through the
// public JSignals so any number of views/selection models stay in sync.
// ----------------------------------------------------------------------------
class JAbstractItemModel {
public:
    virtual ~JAbstractItemModel() = default;

    // ---- Structure ---------------------------------------------------------
    virtual int rowCount(const JModelIndex& parent = JModelIndex()) const = 0;
    virtual int columnCount(const JModelIndex& parent = JModelIndex()) const = 0;

    // Build the index for (row, col) under a parent. Return an invalid index
    // when out of range. Flat models ignore `parent`.
    virtual JModelIndex index(int row, int col,
                              const JModelIndex& parent = JModelIndex()) const = 0;

    // Parent of `child`. Flat models return an invalid (root) index.
    virtual JModelIndex parent(const JModelIndex& /*child*/) const {
        return JModelIndex();
    }

    bool hasChildren(const JModelIndex& parent = JModelIndex()) const {
        return rowCount(parent) > 0 && columnCount(parent) > 0;
    }
    bool hasIndex(int row, int col, const JModelIndex& parent = JModelIndex()) const {
        if (row < 0 || col < 0) return false;
        return row < rowCount(parent) && col < columnCount(parent);
    }

    // ---- Data --------------------------------------------------------------
    virtual JVariant data(const JModelIndex& index,
                          int role = roleId(JItemRole::Display)) const = 0;

    // Returns true and emits dataChanged on success; default is read-only.
    virtual bool setData(const JModelIndex& /*index*/, const JVariant& /*value*/,
                         int /*role*/ = roleId(JItemRole::Edit)) {
        return false;
    }

    virtual JItemFlag flags(const JModelIndex& index) const {
        if (!index.isValid()) return JItemFlag::None;
        return JItemFlag::Selectable | JItemFlag::Enabled;
    }

    virtual JVariant headerData(int section, JOrientation2 orientation,
                                int role = roleId(JItemRole::Display)) const {
        (void)orientation;
        if (role == roleId(JItemRole::Display)) return JVariant(section + 1);
        return JVariant();
    }

    // ---- Change notification ----------------------------------------------
    // dataChanged(topLeft, bottomRight): inclusive rectangle of changed cells.
    jf::JSignal<JModelIndex, JModelIndex> dataChanged;
    // rows{Inserted,Removed}(parent, firstRow, lastRow): structural change.
    jf::JSignal<JModelIndex, int, int>    rowsInserted;
    jf::JSignal<JModelIndex, int, int>    rowsRemoved;
    // modelReset: wholesale invalidation (all previous indexes are stale).
    jf::JSignal<>                          modelReset;

protected:
    // Factory for subclasses — stamps the model pointer onto the index so that
    // JModelIndex::parent()/data() can call back into us.
    JModelIndex createIndex(int row, int col, void* ptr = nullptr) const {
        return JModelIndex(row, col, ptr, 0, this);
    }
    JModelIndex createIndex(int row, int col, uint64_t id) const {
        return JModelIndex(row, col, nullptr, id, this);
    }

    // Emit helpers keep call sites terse and intention-revealing.
    void emitDataChanged(const JModelIndex& tl, const JModelIndex& br) {
        dataChanged.emit(tl, br);
    }
    void emitDataChanged(const JModelIndex& one) { dataChanged.emit(one, one); }
    void emitRowsInserted(const JModelIndex& parent, int first, int last) {
        rowsInserted.emit(parent, first, last);
    }
    void emitRowsRemoved(const JModelIndex& parent, int first, int last) {
        rowsRemoved.emit(parent, first, last);
    }
    void emitModelReset() { modelReset.emit(); }
};

// ---- JModelIndex members needing the full model type -----------------------
inline JModelIndex JModelIndex::parent() const {
    return m_model ? m_model->parent(*this) : JModelIndex();
}
inline JModelIndex JModelIndex::sibling(int row, int column) const {
    if (!m_model) return JModelIndex();
    return m_model->index(row, column, parent());
}
inline JVariant JModelIndex::data(int role) const {
    return m_model ? m_model->data(*this, role) : JVariant();
}

// ============================================================================
// JStringListModel — flat, single-column, editable list of strings. The
// canonical "hello world" model and a ready fixture for list views.
// ============================================================================
class JStringListModel : public JAbstractItemModel {
public:
    JStringListModel() = default;
    explicit JStringListModel(std::vector<std::string> items)
        : m_items(std::move(items)) {}

    int rowCount(const JModelIndex& parent = JModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_items.size());
    }
    int columnCount(const JModelIndex& parent = JModelIndex()) const override {
        return parent.isValid() ? 0 : 1;
    }
    JModelIndex index(int row, int col,
                      const JModelIndex& parent = JModelIndex()) const override {
        if (parent.isValid() || col != 0 || row < 0 || row >= (int)m_items.size())
            return JModelIndex();
        return createIndex(row, col);
    }

    JVariant data(const JModelIndex& index,
                  int role = roleId(JItemRole::Display)) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= (int)m_items.size())
            return JVariant();
        if (role == roleId(JItemRole::Display) || role == roleId(JItemRole::Edit))
            return JVariant(m_items[index.row()]);
        return JVariant();
    }

    bool setData(const JModelIndex& index, const JVariant& value,
                 int role = roleId(JItemRole::Edit)) override {
        if (!index.isValid() || index.row() < 0 || index.row() >= (int)m_items.size())
            return false;
        if (role != roleId(JItemRole::Edit) && role != roleId(JItemRole::Display))
            return false;
        m_items[index.row()] = value.toString();
        emitDataChanged(index);
        return true;
    }

    JItemFlag flags(const JModelIndex& index) const override {
        if (!index.isValid()) return JItemFlag::None;
        return JItemFlag::Selectable | JItemFlag::Editable | JItemFlag::Enabled;
    }

    // ---- Container-style mutation (each fires the right signal) ------------
    const std::vector<std::string>& stringList() const { return m_items; }

    void setStringList(std::vector<std::string> items) {
        m_items = std::move(items);
        emitModelReset();
    }
    void append(std::string s) {
        int row = static_cast<int>(m_items.size());
        m_items.push_back(std::move(s));
        emitRowsInserted(JModelIndex(), row, row);
    }
    void insert(int row, std::string s) {
        if (row < 0 || row > (int)m_items.size()) return;
        m_items.insert(m_items.begin() + row, std::move(s));
        emitRowsInserted(JModelIndex(), row, row);
    }
    void removeRow(int row) {
        if (row < 0 || row >= (int)m_items.size()) return;
        m_items.erase(m_items.begin() + row);
        emitRowsRemoved(JModelIndex(), row, row);
    }

private:
    std::vector<std::string> m_items;
};

// ============================================================================
// JGridModel — flat 2D grid of JVariant. A dense, editable table backing store
// and a fixture for table views. Optional per-orientation header labels.
// ============================================================================
class JGridModel : public JAbstractItemModel {
public:
    JGridModel() = default;
    JGridModel(int rows, int cols) { resize(rows, cols); }

    void resize(int rows, int cols) {
        m_rows = rows < 0 ? 0 : rows;
        m_cols = cols < 0 ? 0 : cols;
        m_cells.assign(static_cast<size_t>(m_rows) * m_cols, JVariant());
        m_hHeaders.assign(m_cols, JVariant());
        m_vHeaders.assign(m_rows, JVariant());
        emitModelReset();
    }

    int rowCount(const JModelIndex& parent = JModelIndex()) const override {
        return parent.isValid() ? 0 : m_rows;
    }
    int columnCount(const JModelIndex& parent = JModelIndex()) const override {
        return parent.isValid() ? 0 : m_cols;
    }
    JModelIndex index(int row, int col,
                      const JModelIndex& parent = JModelIndex()) const override {
        if (parent.isValid() || !_inRange(row, col)) return JModelIndex();
        return createIndex(row, col);
    }

    JVariant data(const JModelIndex& index,
                  int role = roleId(JItemRole::Display)) const override {
        if (!index.isValid() || !_inRange(index.row(), index.column()))
            return JVariant();
        if (role == roleId(JItemRole::Display) || role == roleId(JItemRole::Edit))
            return m_cells[_at(index.row(), index.column())];
        return JVariant();
    }

    bool setData(const JModelIndex& index, const JVariant& value,
                 int role = roleId(JItemRole::Edit)) override {
        if (!index.isValid() || !_inRange(index.row(), index.column()))
            return false;
        if (role != roleId(JItemRole::Edit) && role != roleId(JItemRole::Display))
            return false;
        m_cells[_at(index.row(), index.column())] = value;
        emitDataChanged(index);
        return true;
    }

    JItemFlag flags(const JModelIndex& index) const override {
        if (!index.isValid()) return JItemFlag::None;
        return JItemFlag::Selectable | JItemFlag::Editable | JItemFlag::Enabled;
    }

    JVariant headerData(int section, JOrientation2 orientation,
                        int role = roleId(JItemRole::Display)) const override {
        if (role != roleId(JItemRole::Display)) return JVariant();
        const auto& hdr = (orientation == JOrientation2::Horizontal) ? m_hHeaders : m_vHeaders;
        if (section < 0 || section >= (int)hdr.size()) return JVariant(section + 1);
        return hdr[section].isNull() ? JVariant(section + 1) : hdr[section];
    }

    void setHeaderData(int section, JOrientation2 orientation, JVariant label) {
        auto& hdr = (orientation == JOrientation2::Horizontal) ? m_hHeaders : m_vHeaders;
        if (section >= 0 && section < (int)hdr.size()) hdr[section] = std::move(label);
    }

private:
    bool   _inRange(int r, int c) const { return r >= 0 && r < m_rows && c >= 0 && c < m_cols; }
    size_t _at(int r, int c) const { return static_cast<size_t>(r) * m_cols + c; }

    int m_rows = 0;
    int m_cols = 0;
    std::vector<JVariant> m_cells;     // row-major
    std::vector<JVariant> m_hHeaders;  // per column
    std::vector<JVariant> m_vHeaders;  // per row
};

}  // inline namespace jf
