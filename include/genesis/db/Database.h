#pragma once

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <mutex>
#include <thread>
#include <sqlite3.h>

#include <genesis/core/MainThreadDispatcher.h>

// JDatabase.h requires linking against sqlite3:
//   target_link_libraries(myapp PRIVATE sqlite3)

namespace Genesis {

// ============================================================================
// JDatabase — thread-safe SQLite3 wrapper
//
// All synchronous methods are mutex-protected — safe to call from any thread.
// Async variants run the query on a background thread and fire the callback
// on the main thread via JMainThreadDispatcher, so UI updates are safe.
//
// Synchronous usage (any thread):
//   JDatabase db;
//   db.open("runs.db");
//   db.exec("CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY, name TEXT)");
//   db.exec("INSERT INTO runs (name) VALUES (?)", {JBind::from("run1")});
//   db.query("SELECT * FROM runs", {}, [](const JDatabase::JRow& r) {
//       printf("%lld  %s\n", r.integer(0), r.text(1).c_str());
//   });
//
// Async usage (background write, main-thread callback):
//   db.execAsync("INSERT INTO telemetry VALUES (?,?)", {JBind::from(ts), JBind::from(val)});
//   db.queryAsync("SELECT * FROM runs", {}, [](std::vector<JDatabase::JRowSnapshot> rows) {
//       for (auto& r : rows) printf("%s\n", r.text(1).c_str());
//   });
// ============================================================================
class JDatabase {
public:
    // ---- Live row view — valid only inside a synchronous query() callback ----
    class JRow {
    public:
        explicit JRow(sqlite3_stmt* s) : m_stmt(s) {}
        int         columns()           const { return sqlite3_column_count(m_stmt); }
        std::string text(int col)       const {
            auto* p = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, col));
            return p ? p : "";
        }
        double      real(int col)       const { return sqlite3_column_double(m_stmt, col); }
        int64_t     integer(int col)    const { return sqlite3_column_int64(m_stmt, col); }
        bool        isNull(int col)     const { return sqlite3_column_type(m_stmt, col) == SQLITE_NULL; }
        std::string columnName(int col) const {
            auto* p = sqlite3_column_name(m_stmt, col);
            return p ? p : "";
        }
    private:
        sqlite3_stmt* m_stmt;
    };

    // ---- Materialized row — used by queryAsync callbacks ----
    // Copied off the SQLite statement while still on the background thread;
    // safe to read on the main thread after the query completes.
    struct JRowSnapshot {
        struct JCell {
            int         type{SQLITE_NULL}; // SQLITE_INTEGER / SQLITE_FLOAT / SQLITE_TEXT / SQLITE_NULL
            int64_t     ival{0};
            double      dval{0.0};
            std::string sval;
            std::string name;
        };
        std::vector<JCell> cells;

        int         columns()           const { return static_cast<int>(cells.size()); }
        std::string text(int col)       const { return cells[col].sval; }
        double      real(int col)       const { return cells[col].dval; }
        int64_t     integer(int col)    const { return cells[col].ival; }
        bool        isNull(int col)     const { return cells[col].type == SQLITE_NULL; }
        std::string columnName(int col) const { return cells[col].name; }

        static JRowSnapshot from(const JRow& r) {
            JRowSnapshot s;
            int n = r.columns();
            s.cells.resize(n);
            for (int i = 0; i < n; ++i) {
                s.cells[i].name = r.columnName(i);
                s.cells[i].sval = r.text(i);
                s.cells[i].dval = r.real(i);
                s.cells[i].ival = r.integer(i);
                s.cells[i].type = r.isNull(i) ? SQLITE_NULL : SQLITE_TEXT;
            }
            return s;
        }
    };

    // ---- JBind parameter ----
    struct JBind {
        enum class JKind { Null, Int, Real, Text } kind{JKind::Null};
        int64_t     ival{0};
        double      dval{0};
        std::string sval;
        static JBind null()               { return {JKind::Null, 0, 0, {}}; }
        static JBind from(int64_t v)      { return {JKind::Int,  v, 0, {}}; }
        static JBind from(int v)          { return {JKind::Int,  v, 0, {}}; }
        static JBind from(double v)       { return {JKind::Real, 0, v, {}}; }
        static JBind from(std::string v)  { JBind b; b.kind=JKind::Text; b.sval=std::move(v); return b; }
        static JBind from(const char* v)  { return from(std::string(v ? v : "")); }
    };

    JDatabase()  = default;
    ~JDatabase() { close(); }

    JDatabase(const JDatabase&)            = delete;
    JDatabase& operator=(const JDatabase&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    bool open(const std::string& path) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        _closeUnlocked();
        int rc = sqlite3_open(path.c_str(), &m_db);
        if (rc != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            sqlite3_close(m_db);
            m_db = nullptr;
            return false;
        }
        sqlite3_busy_timeout(m_db, 5000);
        _execUnlocked("PRAGMA journal_mode=WAL");
        _execUnlocked("PRAGMA synchronous=NORMAL");
        return true;
    }

    void close() {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        _closeUnlocked();
    }

    bool isOpen() const {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return m_db != nullptr;
    }

    // -------------------------------------------------------------------------
    // Synchronous API — safe from any thread
    // -------------------------------------------------------------------------

    bool exec(const std::string& sql) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return _execUnlocked(sql);
    }

    // Parameterised query with per-row callback. Returns row count or -1 on error.
    int query(const std::string& sql,
              std::vector<JBind> params,
              std::function<void(const JRow&)> cb) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return _queryUnlocked(sql, params, cb);
    }

    // Parameterised exec (INSERT / UPDATE / DELETE).
    bool exec(const std::string& sql, std::vector<JBind> params) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return _queryUnlocked(sql, params, nullptr) >= 0;
    }

    int64_t lastInsertRowId() const {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
    }

    int changes() const {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        return m_db ? sqlite3_changes(m_db) : 0;
    }

    const std::string& lastError() const { return m_lastError; }

    // Wraps body() in BEGIN/COMMIT; rolls back on false return or exception.
    bool transaction(std::function<bool()> body) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        _execUnlocked("BEGIN");
        try {
            if (body()) { _execUnlocked("COMMIT");   return true;  }
            else        { _execUnlocked("ROLLBACK"); return false; }
        } catch (...) {
            _execUnlocked("ROLLBACK");
            throw;
        }
    }

    // -------------------------------------------------------------------------
    // Async API — query/exec runs on a background thread; callback fires on
    // the main thread via JMainThreadDispatcher (safe for UI updates).
    // -------------------------------------------------------------------------

    // Runs SELECT on background thread; cb receives all rows on main thread.
    void queryAsync(const std::string& sql,
                    std::vector<JBind> params,
                    std::function<void(std::vector<JRowSnapshot>)> cb) {
        std::thread([this, sql, params = std::move(params), cb]() mutable {
            std::vector<JRowSnapshot> rows;
            {
                std::lock_guard<std::recursive_mutex> lk(m_mutex);
                _queryUnlocked(sql, params, [&rows](const JRow& r) {
                    rows.push_back(JRowSnapshot::from(r));
                });
            }
            if (cb) {
                JMainThreadDispatcher::instance().post(
                    [cb, rows = std::move(rows)]() mutable { cb(std::move(rows)); });
            }
        }).detach();
    }

    // Runs INSERT/UPDATE/DELETE on background thread; optional cb(success) on main thread.
    void execAsync(const std::string& sql,
                   std::vector<JBind> params,
                   std::function<void(bool)> cb = nullptr) {
        std::thread([this, sql, params = std::move(params), cb]() mutable {
            bool ok;
            {
                std::lock_guard<std::recursive_mutex> lk(m_mutex);
                ok = _queryUnlocked(sql, params, nullptr) >= 0;
            }
            if (cb) {
                JMainThreadDispatcher::instance().post([cb, ok]() { cb(ok); });
            }
        }).detach();
    }

    // Runs a batch of writes in a single transaction on a background thread.
    void transactionAsync(std::function<bool()> body,
                          std::function<void(bool)> cb = nullptr) {
        std::thread([this, body = std::move(body), cb]() mutable {
            bool ok;
            {
                std::lock_guard<std::recursive_mutex> lk(m_mutex);
                _execUnlocked("BEGIN");
                try {
                    ok = body();
                    _execUnlocked(ok ? "COMMIT" : "ROLLBACK");
                } catch (...) {
                    _execUnlocked("ROLLBACK");
                    ok = false;
                }
            }
            if (cb) {
                JMainThreadDispatcher::instance().post([cb, ok]() { cb(ok); });
            }
        }).detach();
    }

private:
    mutable std::recursive_mutex m_mutex;
    sqlite3*           m_db{nullptr};
    std::string        m_lastError;

    void _closeUnlocked() {
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
    }

    bool _execUnlocked(const std::string& sql) {
        if (!m_db) { m_lastError = "database not open"; return false; }
        char* err = nullptr;
        int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            m_lastError = err ? err : "unknown error";
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    int _queryUnlocked(const std::string& sql,
                       const std::vector<JBind>& params,
                       std::function<void(const JRow&)> cb) {
        if (!m_db) { m_lastError = "database not open"; return -1; }
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) { m_lastError = sqlite3_errmsg(m_db); return -1; }
        _bindUnlocked(stmt, params);
        int rows = 0;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (cb) cb(JRow(stmt));
            ++rows;
        }
        if (rc != SQLITE_DONE) m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE) ? rows : -1;
    }

    static void _bindUnlocked(sqlite3_stmt* s, const std::vector<JBind>& params) {
        for (int i = 0; i < static_cast<int>(params.size()); ++i) {
            const auto& p = params[i];
            switch (p.kind) {
                case JBind::JKind::Null: sqlite3_bind_null(s, i+1);                                         break;
                case JBind::JKind::Int:  sqlite3_bind_int64(s, i+1, p.ival);                               break;
                case JBind::JKind::Real: sqlite3_bind_double(s, i+1, p.dval);                              break;
                case JBind::JKind::Text: sqlite3_bind_text(s, i+1, p.sval.c_str(), -1, SQLITE_TRANSIENT); break;
            }
        }
    }
};

} // namespace Genesis
