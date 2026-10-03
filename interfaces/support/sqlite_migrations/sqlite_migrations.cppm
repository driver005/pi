export module pi.support.sqlite_migrations;

import std;
export import pi.durable.i_sql_database;
export import pi.types.result;

/**
 * The ordered schema history of the durable SQLite storage and its atomic application. The schema is the one of
 * packages/durable/src/storage/sqlite/migrations.ts, so a database file written by either implementation opens in the
 * other. New migrations are appended after the initial schema ships; applied ones never change.
 */
export class SqliteMigrations {
public:
    /** The schema version this build writes. */
    std::int64_t currentVersion() const {
        return static_cast<std::int64_t>(m_versions.size());
    }

    /** Applies every migration above the stored version in one transaction; refuses a newer stored version. */
    Result<void> apply(ISqlDatabase& database) const {
        return database.transaction([&]() -> Result<void> {
            if (auto created = database.exec(
                    "CREATE TABLE IF NOT EXISTS durable_schema (singleton INTEGER PRIMARY KEY CHECK (singleton = 1), "
                    "version INTEGER NOT NULL CHECK (version >= 0)) STRICT");
                !created) {
                return created;
            }
            if (auto seeded = database.run("INSERT OR IGNORE INTO durable_schema (singleton, version) VALUES (1, 0)", {}); !seeded) {
                return seeded;
            }
            auto row = database.get("SELECT version FROM durable_schema WHERE singleton = 1", {});
            if (!row) {
                return std::unexpected(row.error());
            }
            if (!*row) {
                return std::unexpected(Error{"storage_error", "Durable SQLite schema metadata is missing"});
            }
            const std::int64_t stored = std::get<std::int64_t>((**row).at("version"));
            if (stored > currentVersion()) {
                return std::unexpected(Error{"storage_error", "Durable SQLite schema version " + std::to_string(stored) +
                                                                  " is newer than supported version " + std::to_string(currentVersion())});
            }
            for (std::int64_t version = stored + 1; version <= currentVersion(); ++version) {
                for (const std::string& statement : m_versions[static_cast<std::size_t>(version - 1)]) {
                    if (auto done = database.exec(statement); !done) {
                        return done;
                    }
                }
                if (auto marked = database.run("UPDATE durable_schema SET version = ? WHERE singleton = 1", {version}); !marked) {
                    return marked;
                }
            }
            return {};
        });
    }

private:
    // next_id is TEXT so that ids beyond the safe integer range of other bindings survive.
    const std::vector<std::vector<std::string>> m_versions = {{
        "CREATE TABLE durable_metadata (singleton INTEGER PRIMARY KEY CHECK (singleton = 1), next_id TEXT NOT NULL, "
        "next_seq INTEGER NOT NULL) STRICT",
        "INSERT INTO durable_metadata (singleton, next_id, next_seq) VALUES (1, '2', 1)",
        "CREATE TABLE record_ids (id INTEGER PRIMARY KEY, record_type TEXT NOT NULL CHECK (record_type IN "
        "('conversation', 'entry', 'task', 'submission', 'document'))) STRICT",
        "CREATE TABLE conversations (id INTEGER PRIMARY KEY, owner_conversation_id INTEGER, owner_task_id INTEGER, "
        "record TEXT NOT NULL CHECK (json_valid(record))) STRICT",
        "CREATE INDEX conversations_by_owner_conversation ON conversations (owner_conversation_id, id)",
        "CREATE INDEX conversations_by_owner_task ON conversations (owner_task_id, id)",
        "CREATE TABLE entries (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, head INTEGER, "
        "commit_seq INTEGER NOT NULL, record TEXT NOT NULL CHECK (json_valid(record))) STRICT",
        "CREATE INDEX entries_by_conversation ON entries (conversation_id, id DESC)",
        "CREATE INDEX entry_heads_by_conversation ON entries (conversation_id, id DESC) WHERE head IS NOT NULL",
        "CREATE TABLE tasks (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, kind TEXT NOT NULL, "
        "status TEXT NOT NULL CHECK (status IN ('pending', 'running', 'waiting', 'completing', 'terminal')), "
        "abort_requested INTEGER NOT NULL CHECK (abort_requested IN (0, 1)), "
        "background INTEGER NOT NULL CHECK (background IN (0, 1)), "
        "record TEXT NOT NULL CHECK (json_valid(record))) STRICT",
        "CREATE INDEX tasks_by_status ON tasks (status, id)",
        "CREATE INDEX tasks_by_conversation ON tasks (conversation_id, id)",
        "CREATE INDEX tasks_by_kind ON tasks (kind, id)",
        "CREATE INDEX tasks_by_abort_requested ON tasks (abort_requested, id)",
        "CREATE INDEX tasks_by_background ON tasks (background, id)",
        "CREATE TABLE submissions (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, request_id TEXT, "
        "status TEXT NOT NULL CHECK (status IN ('queued', 'placed', 'done', 'unanswered')), "
        "record TEXT NOT NULL CHECK (json_valid(record))) STRICT",
        "CREATE INDEX submissions_by_request ON submissions (conversation_id, request_id)",
        "CREATE INDEX submissions_by_conversation ON submissions (conversation_id, id)",
        "CREATE INDEX submissions_by_status ON submissions (status, id)",
        "CREATE TABLE documents (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, "
        "family INTEGER NOT NULL CHECK (family IN (0, 1)), key_value TEXT NOT NULL, "
        "scope_kind TEXT NOT NULL CHECK (scope_kind IN ('session', 'conversation', 'task')), owner_id INTEGER NOT NULL, "
        "created_at INTEGER NOT NULL, retired_at INTEGER, record TEXT NOT NULL CHECK (json_valid(record))) STRICT",
        "CREATE INDEX documents_by_address ON documents (kind, scope_kind, owner_id, family, key_value, created_at DESC, "
        "retired_at)",
        "CREATE INDEX documents_by_scope ON documents (scope_kind, owner_id, id)",
        "CREATE INDEX documents_by_scope_kind ON documents (scope_kind, owner_id, kind, id)",
        "CREATE TABLE document_revisions (document_id INTEGER NOT NULL, seq INTEGER NOT NULL, "
        "kind TEXT NOT NULL CHECK (kind IN ('base', 'delta')), version INTEGER NOT NULL, "
        "content TEXT NOT NULL CHECK (json_valid(content)), PRIMARY KEY (document_id, seq)) STRICT",
        "CREATE INDEX document_revisions_by_kind ON document_revisions (document_id, kind, seq DESC)",
    }};
};
