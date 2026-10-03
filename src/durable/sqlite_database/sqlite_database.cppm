module;

#include <sqlite3.h>

#include <cstdint>

export module pi.durable.sqlite_database;

import std;
export import pi.durable.i_sql_database;
export import pi.types.sqlite_options;

/**
 * ISqlDatabase over one SQLite connection (the system libsqlite3). Prepared statements are cached by SQL text; every
 * operation holds one recursive lock, so a transaction keeps other threads out until it ends and its own work can use
 * the database freely. Port of the node adapter in packages/durable/src/storage/sqlite/node.ts: WAL journal, NORMAL
 * synchronous mode, immediate transactions, a truncating checkpoint on close.
 */
export class SqliteDatabase : public ISqlDatabase {
public:
    /** `path` is a file (its directory is created) or ":memory:". Call open() before use. */
    explicit SqliteDatabase(std::string path, SqliteOptions options = {})
        : m_path(std::move(path)),
          m_options(options) {}

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    ~SqliteDatabase() override {
        (void)close();
    }

    Result<void> open() {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_db != nullptr) {
            return {};
        }
        if (m_path != ":memory:") {
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), error);
            if (error) {
                return std::unexpected(Error{"storage_error", "Cannot create " + m_path + ": " + error.message()});
            }
        }
        sqlite3* db = nullptr;
        const int code = sqlite3_open_v2(m_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (code != SQLITE_OK) {
            const std::string message = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code);
            sqlite3_close_v2(db);
            return std::unexpected(Error{"storage_error", "Cannot open " + m_path + ": " + message});
        }
        m_db = db;
        m_closed = false;
        sqlite3_busy_timeout(m_db, m_options.busyTimeoutMs);
        for (const std::string& pragma : {std::string("PRAGMA journal_mode = WAL"), std::string("PRAGMA synchronous = NORMAL"),
                                          "PRAGMA wal_autocheckpoint = " + std::to_string(m_options.walAutoCheckpointPages)}) {
            if (auto done = exec(pragma); !done) {
                (void)closeLocked();
                return done;
            }
        }
        return {};
    }

    Result<void> exec(const std::string& sql) override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (auto usable = usableLocked(); !usable) {
            return usable;
        }
        char* message = nullptr;
        if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            const std::string text = message != nullptr ? message : sqlite3_errmsg(m_db);
            sqlite3_free(message);
            return std::unexpected(Error{"storage_error", text});
        }
        return {};
    }

    Result<void> run(const std::string& sql, const std::vector<SqlValue>& params) override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto rows = query(sql, params, 0);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        return {};
    }

    Result<std::optional<SqlRow>> get(const std::string& sql, const std::vector<SqlValue>& params) override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto rows = query(sql, params, 1);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        if (rows->empty()) {
            return std::optional<SqlRow>();
        }
        return std::optional<SqlRow>(std::move(rows->front()));
    }

    Result<std::vector<SqlRow>> all(const std::string& sql, const std::vector<SqlValue>& params) override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return query(sql, params, std::numeric_limits<std::size_t>::max());
    }

    Result<void> transaction(const std::function<Result<void>()>& work) override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_inTransaction) {
            return std::unexpected(Error{"storage_error", "SQLite transactions cannot be nested"});
        }
        if (auto begun = exec("BEGIN IMMEDIATE"); !begun) {
            return begun;
        }
        m_inTransaction = true;
        Result<void> outcome = work();
        if (outcome) {
            outcome = exec("COMMIT");
        }
        m_inTransaction = false;
        if (!outcome) {
            if (auto rolledBack = exec("ROLLBACK"); !rolledBack) {
                return std::unexpected(Error{"storage_error", outcome.error().message + "; rollback also failed: " + rolledBack.error().message});
            }
        }
        return outcome;
    }

    Result<void> close() override {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return closeLocked();
    }

private:
    Result<void> usableLocked() const {
        if (m_db == nullptr || m_closed) {
            return std::unexpected(Error{"storage_error", "SQLite database is closed"});
        }
        return {};
    }

    Result<void> closeLocked() {
        if (m_closed || m_db == nullptr) {
            m_closed = true;
            return {};
        }
        m_closed = true;
        for (auto& entry : m_statements) {
            sqlite3_finalize(entry.second);
        }
        m_statements.clear();
        // A failed checkpoint leaves the log for the next open to replay.
        sqlite3_exec(m_db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
        const int code = sqlite3_close_v2(m_db);
        m_db = nullptr;
        if (code != SQLITE_OK) {
            return std::unexpected(Error{"storage_error", sqlite3_errstr(code)});
        }
        return {};
    }

    Result<sqlite3_stmt*> statement(const std::string& sql) {
        auto found = m_statements.find(sql);
        if (found != m_statements.end()) {
            return found->second;
        }
        sqlite3_stmt* prepared = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &prepared, nullptr) != SQLITE_OK) {
            return std::unexpected(Error{"storage_error", std::string(sqlite3_errmsg(m_db)) + " in: " + sql});
        }
        m_statements.emplace(sql, prepared);
        return prepared;
    }

    Result<void> bind(sqlite3_stmt* prepared, const std::vector<SqlValue>& params) const {
        for (std::size_t index = 0; index < params.size(); ++index) {
            const int position = static_cast<int>(index) + 1;
            int code = SQLITE_OK;
            if (std::holds_alternative<std::int64_t>(params[index])) {
                code = sqlite3_bind_int64(prepared, position, std::get<std::int64_t>(params[index]));
            } else if (std::holds_alternative<std::string>(params[index])) {
                const std::string& text = std::get<std::string>(params[index]);
                code = sqlite3_bind_text(prepared, position, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
            } else {
                code = sqlite3_bind_null(prepared, position);
            }
            if (code != SQLITE_OK) {
                return std::unexpected(Error{"storage_error", sqlite3_errstr(code)});
            }
        }
        return {};
    }

    SqlRow read(sqlite3_stmt* prepared) const {
        SqlRow row;
        const int columns = sqlite3_column_count(prepared);
        for (int column = 0; column < columns; ++column) {
            const std::string name = sqlite3_column_name(prepared, column);
            switch (sqlite3_column_type(prepared, column)) {
                case SQLITE_INTEGER:
                    row[name] = static_cast<std::int64_t>(sqlite3_column_int64(prepared, column));
                    break;
                case SQLITE_NULL:
                    row[name] = std::monostate{};
                    break;
                default: {
                    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(prepared, column));
                    row[name] = std::string(text != nullptr ? text : "", static_cast<std::size_t>(sqlite3_column_bytes(prepared, column)));
                    break;
                }
            }
        }
        return row;
    }

    /** Executes one statement and collects at most `limit` rows (the statement is always run to completion or reset). */
    Result<std::vector<SqlRow>> query(const std::string& sql, const std::vector<SqlValue>& params, std::size_t limit) {
        if (auto usable = usableLocked(); !usable) {
            return std::unexpected(usable.error());
        }
        auto prepared = statement(sql);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        sqlite3_reset(*prepared);
        sqlite3_clear_bindings(*prepared);
        if (auto bound = bind(*prepared, params); !bound) {
            return std::unexpected(bound.error());
        }
        std::vector<SqlRow> rows;
        int code = sqlite3_step(*prepared);
        while (code == SQLITE_ROW) {
            if (rows.size() < limit) {
                rows.push_back(read(*prepared));
            }
            code = rows.size() >= limit && limit != std::numeric_limits<std::size_t>::max() ? SQLITE_DONE : sqlite3_step(*prepared);
        }
        const std::string message = code == SQLITE_DONE ? "" : sqlite3_errmsg(m_db);
        sqlite3_reset(*prepared);
        if (code != SQLITE_DONE) {
            return std::unexpected(Error{"storage_error", message + " in: " + sql});
        }
        return rows;
    }

    std::string m_path;
    SqliteOptions m_options;
    std::recursive_mutex m_mutex;
    sqlite3* m_db = nullptr;
    bool m_closed = false;
    bool m_inTransaction = false;
    std::map<std::string, sqlite3_stmt*> m_statements;
};
