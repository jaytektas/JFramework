#pragma once

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <sqlite3.h>

// Database.h requires linking against sqlite3:
//   target_link_libraries(myapp PRIVATE sqlite3)

namespace Genesis {

// ============================================================================
// Database — thin SQLite3 wrapper (Step 7)
//
// Superior to Qt's QSqlDatabase: no driver plugin system, no QVariant boxing,
// no sql/ module dependency. Rows are plain C++ structs via a Row view.
//
// Usage:
//   Database db;
//   db.open(":memory:");
//   db.exec("CREATE TABLE t (id INTEGER, name TEXT)");
//   db.exec("INSERT INTO t VALUES (1, 'hello')");
//   db.query("SELECT * FROM t WHERE id = ?", {1}, [](const Database::Row& r) {
//       printf("%d  %s\n", r.integer(0), r.text(1).c_str());
//   });
// ============================================================================
class Database {
public:
    class Row {
    public:
        explicit Row(sqlite3_stmt* s) : m_stmt(s) {}
        int         columns()          const { return sqlite3_column_count(m_stmt); }
        std::string text(int col)      const {
            auto* p = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, col));
            return p ? p : "";
        }
        double      real(int col)      const { return sqlite3_column_double(m_stmt, col); }
        int64_t     integer(int col)   const { return sqlite3_column_int64(m_stmt, col); }
        bool        isNull(int col)    const { return sqlite3_column_type(m_stmt, col) == SQLITE_NULL; }
        std::string columnName(int col)const {
            auto* p = sqlite3_column_name(m_stmt, col);
            return p ? p : "";
        }
    private:
        sqlite3_stmt* m_stmt;
    };

    // ---- Bind variants ----
    struct Bind {
        enum class Kind { Null, Int, Real, Text } kind{Kind::Null};
        int64_t     ival{0};
        double      dval{0};
        std::string sval;
        static Bind null()              { return {Kind::Null, 0, 0, {}}; }
        static Bind from(int64_t v)     { return {Kind::Int,  v, 0, {}}; }
        static Bind from(int v)         { return {Kind::Int,  v, 0, {}}; }
        static Bind from(double v)      { return {Kind::Real, 0, v, {}}; }
        static Bind from(std::string v) { Bind b; b.kind=Kind::Text; b.sval=std::move(v); return b; }
        static Bind from(const char* v) { return from(std::string(v ? v : "")); }
    };

    Database() = default;
    ~Database() { close(); }

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path) {
        close();
        int rc = sqlite3_open(path.c_str(), &m_db);
        if (rc != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            sqlite3_close(m_db);
            m_db = nullptr;
            return false;
        }
        sqlite3_busy_timeout(m_db, 5000);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
        return true;
    }

    void close() {
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
    }

    bool isOpen() const { return m_db != nullptr; }

    // Simple no-bind exec (CREATE TABLE, PRAGMA, etc.)
    bool exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            m_lastError = err ? err : "unknown error";
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    // Parameterised query with row callback. Returns number of rows visited.
    int query(const std::string& sql,
              std::vector<Bind> params,
              std::function<void(const Row&)> cb) {
        sqlite3_stmt* stmt = _prepare(sql);
        if (!stmt) return -1;
        _bind(stmt, params);
        int rows = 0;
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (cb) cb(Row(stmt));
            ++rows;
        }
        if (rc != SQLITE_DONE) m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return rows;
    }

    // Exec with params but no row callback (INSERT/UPDATE/DELETE).
    bool exec(const std::string& sql, std::vector<Bind> params) {
        return query(sql, std::move(params), nullptr) >= 0;
    }

    // Last row id after an INSERT.
    int64_t lastInsertRowId() const {
        return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
    }

    // Rows affected by last UPDATE/DELETE.
    int changes() const {
        return m_db ? sqlite3_changes(m_db) : 0;
    }

    const std::string& lastError() const { return m_lastError; }

    // Convenience: wrap a block in BEGIN/COMMIT, ROLLBACK on exception.
    bool transaction(std::function<bool()> body) {
        exec("BEGIN");
        try {
            if (body()) { exec("COMMIT");   return true;  }
            else        { exec("ROLLBACK"); return false; }
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
    }

private:
    sqlite3* m_db{nullptr};
    std::string m_lastError;

    sqlite3_stmt* _prepare(const std::string& sql) {
        sqlite3_stmt* s = nullptr;
        int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &s, nullptr);
        if (rc != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return nullptr;
        }
        return s;
    }

    static void _bind(sqlite3_stmt* s, const std::vector<Bind>& params) {
        for (int i = 0; i < static_cast<int>(params.size()); ++i) {
            const auto& p = params[i];
            switch (p.kind) {
                case Bind::Kind::Null: sqlite3_bind_null(s, i+1);                                         break;
                case Bind::Kind::Int:  sqlite3_bind_int64(s, i+1, p.ival);                               break;
                case Bind::Kind::Real: sqlite3_bind_double(s, i+1, p.dval);                              break;
                case Bind::Kind::Text: sqlite3_bind_text(s, i+1, p.sval.c_str(), -1, SQLITE_TRANSIENT); break;
            }
        }
    }
};

} // namespace Genesis
