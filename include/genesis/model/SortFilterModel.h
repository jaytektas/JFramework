#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <genesis/core/Signal.h>
#include <genesis/model/TableModel.h>

inline namespace jf {

// ============================================================================
// JSortFilterModel — sort and filter proxy over a JTableModel.
//
// Lives on the main thread. Subscribes to source.onChanged, rebuilds the
// index mapping on every change, and emits its own onChanged.
//
// The source rows are snapshotted on each rebuild so row()/rows() access is
// cheap and lock-free after the rebuild.
//
// Usage:
//   JSortFilterModel proxy(model);
//   proxy.setFilter([](const auto& row){ return row[1] != "0"; });
//   proxy.sort(0);  // sort by column 0 ascending
//
//   // JBind to a JDataGrid:
//   auto conn = bindDataGrid(grid, proxy);
// ============================================================================
class JSortFilterModel {
public:
    jf::JSignal<> onChanged;

    explicit JSortFilterModel(JTableModel& source) : m_source(&source) {
        m_conn.addConnection(source.onChanged.connect([this]{
            _rebuild();
            onChanged.emit();
        }));
        _rebuild();
    }

    ~JSortFilterModel() = default;

    JSortFilterModel(const JSortFilterModel&) = delete;
    JSortFilterModel& operator=(const JSortFilterModel&) = delete;

    // ---- Filter -------------------------------------------------------------

    void setFilter(std::function<bool(const std::vector<std::string>&)> pred) {
        m_filter = std::move(pred);
        _rebuild();
        onChanged.emit();
    }

    void clearFilter() {
        m_filter = nullptr;
        _rebuild();
        onChanged.emit();
    }

    // ---- Sort ---------------------------------------------------------------

    void sort(int col, bool ascending = true) {
        m_sortCol = col;
        m_sortAsc = ascending;
        _rebuild();
        onChanged.emit();
    }

    void clearSort() {
        m_sortCol = -1;
        _rebuild();
        onChanged.emit();
    }

    // ---- Data access (post-rebuild snapshot) --------------------------------

    int rowCount() const { return static_cast<int>(m_indices.size()); }

    std::vector<std::string> row(int idx) const {
        if (idx < 0 || idx >= (int)m_indices.size()) return {};
        return _toStr(m_snapshot[m_indices[idx]]);
    }

    std::vector<JVariant> rowVar(int idx) const {
        if (idx < 0 || idx >= (int)m_indices.size()) return {};
        return m_snapshot[m_indices[idx]];
    }

    // Returns the corresponding source row index for a filtered row.
    int sourceRow(int filteredIdx) const {
        if (filteredIdx < 0 || filteredIdx >= (int)m_indices.size()) return -1;
        return m_indices[filteredIdx];
    }

    // Headers are delegated to source.
    std::vector<std::string> headers() const { return m_source->headers(); }

    // Full snapshot of filtered+sorted rows (for JDataGrid.setRows()).
    std::vector<std::vector<std::string>> rows() const {
        std::vector<std::vector<std::string>> result;
        result.reserve(m_indices.size());
        for (int i : m_indices) result.push_back(_toStr(m_snapshot[i]));
        return result;
    }

private:
    JTableModel*                                       m_source{nullptr};
    jf::JSlotTracker                                 m_conn;
    std::vector<std::vector<JVariant>>                 m_snapshot;
    std::vector<int>                                  m_indices;
    std::function<bool(const std::vector<std::string>&)> m_filter;
    int                                               m_sortCol{-1};
    bool                                              m_sortAsc{true};

    static std::vector<std::string> _toStr(const std::vector<JVariant>& r) {
        std::vector<std::string> out;
        out.reserve(r.size());
        for (const auto& c : r) out.push_back(c.toString());
        return out;
    }

    void _rebuild() {
        m_snapshot = m_source->rowsVar();  // single lock, full copy (typed)
        int n = static_cast<int>(m_snapshot.size());
        m_indices.clear();
        m_indices.reserve(n);
        for (int i = 0; i < n; ++i)
            if (!m_filter || m_filter(_toStr(m_snapshot[i])))
                m_indices.push_back(i);
        if (m_sortCol >= 0) {
            std::stable_sort(m_indices.begin(), m_indices.end(),
                [this](int a, int b) {
                    JVariant va = m_sortCol < (int)m_snapshot[a].size()
                                 ? m_snapshot[a][m_sortCol] : JVariant{};
                    JVariant vb = m_sortCol < (int)m_snapshot[b].size()
                                 ? m_snapshot[b][m_sortCol] : JVariant{};
                    return m_sortAsc ? detail::cellLess(va, vb)
                                     : detail::cellLess(vb, va);
                });
        }
    }
};

} // inline namespace jf
