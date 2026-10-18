#pragma once
#include <sqlite3.h>
#include <stdexcept>
#include <string>
namespace tt {
inline void execSql(sqlite3 *db, const char *sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
class Transaction {
    sqlite3 *db_;
    bool active_{true};

  public:
    explicit Transaction(sqlite3 *db) : db_(db) {
        execSql(db_, "BEGIN IMMEDIATE");
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    ~Transaction() {
        if (active_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execSql(db_, "COMMIT");
        active_ = false;
    }
};
class Statement {
    sqlite3 *db_;
    sqlite3_stmt *s{};
    void check(int result) const {
        if (result != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }

  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(s);
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void reset() {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    void text(int n, const std::string &v) {
        check(sqlite3_bind_text(s, n, v.c_str(), -1, SQLITE_TRANSIENT));
    }
    void integer(int n, int64_t v) {
        check(sqlite3_bind_int64(s, n, v));
    }
    bool step() {
        int r = sqlite3_step(s);
        if (r != SQLITE_ROW && r != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db_));
        return r == SQLITE_ROW;
    }
    std::string text(int n) {
        auto p = sqlite3_column_text(s, n);
        return p ? reinterpret_cast<const char *>(p) : "";
    }
    int64_t integer(int n) {
        return sqlite3_column_int64(s, n);
    }
};
} // namespace tt
