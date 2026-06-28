#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <mutex>
#include <memory>
#include <atomic>
#include <genesis/core/Signal.h>
#include <genesis/core/MainThreadDispatcher.h>

namespace Genesis {

// ============================================================================
// TableModel — observable table of string cells.
//
// Thread-safe: mutations from any thread are safe; onChanged always fires on
// the main thread via MainThreadDispatcher.
//
// Usage:
//   TableModel model;
//   model.setHeaders({"Name", "Value", "Unit"});
//   model.append({"Torque", "42.0", "Nm"});
//
//   // Bind to a DataGrid (include <genesis/model/ModelBinding.h>):
//   auto conn = bindDataGrid(grid, model);
//
//   // Bulk insert without intermediate redraws:
//   model.beginBatch();
//   for (auto& r : rows) model.append(r);
//   model.endBatch();  // fires onChanged once
// ============================================================================
class TableModel {
public:
    Core::Signal<> onChanged;    // always fires on the main thread
    Core::Signal<int> onRowInserted;
    Core::Signal<int> onRowRemoved;

    ~TableModel() {
        // Invalidate any callbacks already queued in the dispatcher so they
        // don't fire against this object after it's been destroyed.
        m_alive->store(false, std::memory_order_release);
    }

    // ---- Headers ------------------------------------------------------------

    void setHeaders(std::vector<std::string> headers) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_headers = std::move(headers);
        }
        _notify();
    }

    std::vector<std::string> headers() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_headers;
    }

    int columnCount() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return static_cast<int>(m_headers.size());
    }

    // ---- Row access ---------------------------------------------------------

    int rowCount() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return static_cast<int>(m_rows.size());
    }

    std::vector<std::string> row(int idx) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (idx < 0 || idx >= (int)m_rows.size()) return {};
        return m_rows[idx];
    }

    std::string cell(int row, int col) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (row < 0 || row >= (int)m_rows.size()) return {};
        if (col < 0 || col >= (int)m_rows[row].size()) return {};
        return m_rows[row][col];
    }

    // Full snapshot — used by DataGrid binding.
    std::vector<std::vector<std::string>> rows() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_rows;
    }

    // ---- Mutations ----------------------------------------------------------

    void append(std::vector<std::string> row) {
        int idx;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            idx = static_cast<int>(m_rows.size());
            m_rows.push_back(std::move(row));
        }
        _notifyInserted(idx);
    }

    void insert(int idx, std::vector<std::string> row) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            idx = std::clamp(idx, 0, (int)m_rows.size());
            m_rows.insert(m_rows.begin() + idx, std::move(row));
        }
        _notifyInserted(idx);
    }

    void set(int idx, std::vector<std::string> row) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (idx < 0 || idx >= (int)m_rows.size()) return;
            m_rows[idx] = std::move(row);
        }
        _notify();
    }

    void set(int row, int col, std::string val) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (row < 0 || row >= (int)m_rows.size()) return;
            if (col < 0 || col >= (int)m_rows[row].size()) return;
            m_rows[row][col] = std::move(val);
        }
        _notify();
    }

    void remove(int idx) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (idx < 0 || idx >= (int)m_rows.size()) return;
            m_rows.erase(m_rows.begin() + idx);
        }
        _notifyRemoved(idx);
    }

    void clear() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_rows.clear();
        }
        _notify();
    }

    // ---- Batch operations ---------------------------------------------------
    // Suppress intermediate onChanged signals during bulk inserts.

    void beginBatch() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_batching = true;
        m_pendingChange = false;
    }

    void endBatch() {
        bool pending;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_batching = false;
            pending = m_pendingChange;
            m_pendingChange = false;
        }
        if (pending) _notify();
    }

    // ---- Sort in place ------------------------------------------------------
    // Sorts by string comparison on the given column.
    void sort(int col, bool ascending = true) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            std::stable_sort(m_rows.begin(), m_rows.end(),
                [col, ascending](const auto& a, const auto& b) {
                    const std::string& va = col < (int)a.size() ? a[col] : "";
                    const std::string& vb = col < (int)b.size() ? b[col] : "";
                    return ascending ? (va < vb) : (va > vb);
                });
        }
        _notify();
    }

private:
    mutable std::mutex                           m_mutex;
    std::vector<std::string>                     m_headers;
    std::vector<std::vector<std::string>>        m_rows;
    bool                                         m_batching{false};
    bool                                         m_pendingChange{false};
    std::shared_ptr<std::atomic<bool>>           m_alive{
        std::make_shared<std::atomic<bool>>(true)};

    void _notify() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_batching) { m_pendingChange = true; return; }
        }
        auto alive = m_alive;
        MainThreadDispatcher::instance().post([this, alive]{
            if (alive->load(std::memory_order_acquire)) onChanged.emit();
        });
    }

    void _notifyInserted(int idx) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_batching) { m_pendingChange = true; return; }
        }
        auto alive = m_alive;
        MainThreadDispatcher::instance().post([this, idx, alive]{
            if (!alive->load(std::memory_order_acquire)) return;
            onRowInserted.emit(idx);
            onChanged.emit();
        });
    }

    void _notifyRemoved(int idx) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_batching) { m_pendingChange = true; return; }
        }
        auto alive = m_alive;
        MainThreadDispatcher::instance().post([this, idx, alive]{
            if (!alive->load(std::memory_order_acquire)) return;
            onRowRemoved.emit(idx);
            onChanged.emit();
        });
    }
};

} // namespace Genesis
