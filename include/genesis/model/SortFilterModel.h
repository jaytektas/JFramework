#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <genesis/core/Signal.h>
#include <genesis/model/TableModel.h>

namespace Genesis {

// ============================================================================
// SortFilterModel — sort and filter proxy over a TableModel.
//
// Lives on the main thread. Subscribes to source.onChanged, rebuilds the
// index mapping on every change, and emits its own onChanged.
//
// The source rows are snapshotted on each rebuild so row()/rows() access is
// cheap and lock-free after the rebuild.
//
// Usage:
//   SortFilterModel proxy(model);
//   proxy.setFilter([](const auto& row){ return row[1] != "0"; });
//   proxy.sort(0);  // sort by column 0 ascending
//
//   // Bind to a DataGrid:
//   auto conn = bindDataGrid(grid, proxy);
// ============================================================================
class SortFilterModel {
public:
    Core::Signal<> onChanged;

    explicit SortFilterModel(TableModel& source) : m_source(&source) {
        m_conn.addConnection(source.onChanged.connect([this]{
            _rebuild();
            onChanged.emit();
        }));
        _rebuild();
    }

    ~SortFilterModel() = default;

    SortFilterModel(const SortFilterModel&) = delete;
    SortFilterModel& operator=(const SortFilterModel&) = delete;

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

    const std::vector<std::string>& row(int idx) const {
        static const std::vector<std::string> empty;
        if (idx < 0 || idx >= (int)m_indices.size()) return empty;
        return m_snapshot[m_indices[idx]];
    }

    // Returns the corresponding source row index for a filtered row.
    int sourceRow(int filteredIdx) const {
        if (filteredIdx < 0 || filteredIdx >= (int)m_indices.size()) return -1;
        return m_indices[filteredIdx];
    }

    // Headers are delegated to source.
    std::vector<std::string> headers() const { return m_source->headers(); }

    // Full snapshot of filtered+sorted rows (for DataGrid.setRows()).
    std::vector<std::vector<std::string>> rows() const {
        std::vector<std::vector<std::string>> result;
        result.reserve(m_indices.size());
        for (int i : m_indices) result.push_back(m_snapshot[i]);
        return result;
    }

private:
    TableModel*                                       m_source{nullptr};
    Core::SlotTracker                                 m_conn;
    std::vector<std::vector<std::string>>             m_snapshot;
    std::vector<int>                                  m_indices;
    std::function<bool(const std::vector<std::string>&)> m_filter;
    int                                               m_sortCol{-1};
    bool                                              m_sortAsc{true};

    void _rebuild() {
        m_snapshot = m_source->rows();  // single lock, full copy
        int n = static_cast<int>(m_snapshot.size());
        m_indices.clear();
        m_indices.reserve(n);
        for (int i = 0; i < n; ++i)
            if (!m_filter || m_filter(m_snapshot[i]))
                m_indices.push_back(i);
        if (m_sortCol >= 0) {
            std::stable_sort(m_indices.begin(), m_indices.end(),
                [this](int a, int b) {
                    const auto& va = m_sortCol < (int)m_snapshot[a].size()
                                     ? m_snapshot[a][m_sortCol] : "";
                    const auto& vb = m_sortCol < (int)m_snapshot[b].size()
                                     ? m_snapshot[b][m_sortCol] : "";
                    return m_sortAsc ? (va < vb) : (va > vb);
                });
        }
    }
};

} // namespace Genesis
