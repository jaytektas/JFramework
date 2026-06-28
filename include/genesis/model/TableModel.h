#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <mutex>
#include <memory>
#include <atomic>
#include <optional>
#include <cctype>
#include <genesis/core/Signal.h>
#include <genesis/core/MainThreadDispatcher.h>
#include <genesis/core/Variant.h>

namespace Genesis {

namespace detail {

// Interpret a cell as a number when it is numeric or a fully-numeric string.
inline std::optional<double> cellAsNumber(const Variant& v) {
    if (v.isInt())    return static_cast<double>(v.toInt());
    if (v.isDouble()) return v.toDouble();
    if (v.isBool())   return v.toBool() ? 1.0 : 0.0;
    if (v.isString()) {
        const std::string s = v.toString();
        if (s.empty()) return std::nullopt;
        try {
            size_t pos = 0;
            double d = std::stod(s, &pos);
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            if (pos == s.size()) return d;   // full parse, no trailing junk
        } catch (...) {}
    }
    return std::nullopt;
}

// Numeric-aware cell ordering: compare as numbers when both sides are numeric,
// otherwise fall back to lexicographic string comparison.
inline bool cellLess(const Variant& a, const Variant& b) {
    auto na = cellAsNumber(a), nb = cellAsNumber(b);
    if (na && nb) return *na < *nb;
    return a.toString() < b.toString();
}

}  // namespace detail

// ============================================================================
// TableModel — observable table of Variant cells.
//
// Cells are Variant, so numeric columns sort numerically and typed values
// round-trip through serialization. The string-based API is preserved (cells
// come in/out as strings via Variant coercion); use the *Var accessors when
// you want typed cells (e.g. real numbers for correct sorting).
//
// Thread-safe: mutations from any thread are safe; onChanged always fires on
// the main thread via MainThreadDispatcher.
//
// Usage:
//   TableModel model;
//   model.setHeaders({"Name", "Value", "Unit"});
//   model.append({"Torque", "42.0", "Nm"});          // string row
//   model.appendVar({"Power", 320.5, "kW"});         // typed row (numeric sort)
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
        return _toStr(m_rows[idx]);
    }

    std::string cell(int row, int col) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (row < 0 || row >= (int)m_rows.size()) return {};
        if (col < 0 || col >= (int)m_rows[row].size()) return {};
        return m_rows[row][col].toString();
    }

    // Full snapshot — used by DataGrid binding.
    std::vector<std::vector<std::string>> rows() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<std::vector<std::string>> out;
        out.reserve(m_rows.size());
        for (const auto& r : m_rows) out.push_back(_toStr(r));
        return out;
    }

    // ---- Typed (Variant) access --------------------------------------------

    std::vector<Variant> rowVar(int idx) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (idx < 0 || idx >= (int)m_rows.size()) return {};
        return m_rows[idx];
    }

    Variant cellVar(int row, int col) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (row < 0 || row >= (int)m_rows.size()) return {};
        if (col < 0 || col >= (int)m_rows[row].size()) return {};
        return m_rows[row][col];
    }

    std::vector<std::vector<Variant>> rowsVar() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_rows;
    }

    // ---- Mutations ----------------------------------------------------------

    void append(std::vector<std::string> row)  { appendVar(_toVar(std::move(row))); }
    void insert(int idx, std::vector<std::string> row) { insertVar(idx, _toVar(std::move(row))); }
    void set(int idx, std::vector<std::string> row)    { setVar(idx, _toVar(std::move(row))); }

    void set(int row, int col, Variant val) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (row < 0 || row >= (int)m_rows.size()) return;
            if (col < 0 || col >= (int)m_rows[row].size()) return;
            m_rows[row][col] = std::move(val);
        }
        _notify();
    }

    // ---- Typed (Variant) mutations -----------------------------------------

    void appendVar(std::vector<Variant> row) {
        int idx;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            idx = static_cast<int>(m_rows.size());
            m_rows.push_back(std::move(row));
        }
        _notifyInserted(idx);
    }

    void insertVar(int idx, std::vector<Variant> row) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            idx = std::clamp(idx, 0, (int)m_rows.size());
            m_rows.insert(m_rows.begin() + idx, std::move(row));
        }
        _notifyInserted(idx);
    }

    void setVar(int idx, std::vector<Variant> row) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (idx < 0 || idx >= (int)m_rows.size()) return;
            m_rows[idx] = std::move(row);
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
    // Numeric-aware: numeric columns sort by value, others lexicographically.
    void sort(int col, bool ascending = true) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            std::stable_sort(m_rows.begin(), m_rows.end(),
                [col, ascending](const auto& a, const auto& b) {
                    Variant va = col < (int)a.size() ? a[col] : Variant{};
                    Variant vb = col < (int)b.size() ? b[col] : Variant{};
                    return ascending ? detail::cellLess(va, vb) : detail::cellLess(vb, va);
                });
        }
        _notify();
    }

private:
    mutable std::mutex                           m_mutex;
    std::vector<std::string>                     m_headers;
    std::vector<std::vector<Variant>>            m_rows;

    static std::vector<std::string> _toStr(const std::vector<Variant>& r) {
        std::vector<std::string> out;
        out.reserve(r.size());
        for (const auto& c : r) out.push_back(c.toString());
        return out;
    }
    static std::vector<Variant> _toVar(std::vector<std::string> r) {
        std::vector<Variant> out;
        out.reserve(r.size());
        for (auto& c : r) out.push_back(Variant(std::move(c)));
        return out;
    }
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
