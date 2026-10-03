export module pi.durable.i_sql_database;

import std;
export import pi.types.result;
export import pi.types.sql_row;
export import pi.types.sql_value;

/**
 * The SQL operations the durable SQLite storage needs, blocking. `exec` runs text without bindings that may hold several
 * statements; `run`, `get` and `all` execute one statement with positional bindings (`?`). Implementations may cache
 * prepared statements by SQL text, so callers bind values instead of interpolating them. Counterpart of SqliteDatabase in
 * packages/durable/src/storage/sqlite/database.ts.
 */
export class ISqlDatabase {
public:
    virtual ~ISqlDatabase() = default;

    virtual Result<void> exec(const std::string& sql) = 0;
    virtual Result<void> run(const std::string& sql, const std::vector<SqlValue>& params) = 0;
    virtual Result<std::optional<SqlRow>> get(const std::string& sql, const std::vector<SqlValue>& params) = 0;
    virtual Result<std::vector<SqlRow>> all(const std::string& sql, const std::vector<SqlValue>& params) = 0;

    /**
     * Runs `work` in an immediate transaction: committed when it succeeds, rolled back when it fails (the failure is
     * returned). Other threads' operations wait until it ends. `work` must not start another transaction or close the
     * database.
     */
    virtual Result<void> transaction(const std::function<Result<void>()>& work) = 0;

    /** Checkpoints and releases the database; every later call fails. */
    virtual Result<void> close() = 0;
};
