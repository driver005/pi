export module pi.durable.sqlite_storage;

import std;
export import pi.durable.i_sql_database;
export import pi.durable.i_storage;
export import pi.support.sqlite_migrations;
export import pi.types.document_action;
export import pi.types.fork_parent;
import pi.support.delta_applier;

/**
 * Portable SQLite implementation of the storage contract over an ISqlDatabase. Records are JSON text next to the columns
 * their scans filter on; every commit is one immediate transaction, so a failed commit changes nothing. Same schema and
 * row encoding as packages/durable/src/storage/sqlite/storage.ts (identity strings are JSON-encoded, ids may exceed what
 * other bindings read as integers), so a file written by the TS server opens here and vice versa. Error messages match the
 * TS implementation and the in-memory storage so the conformance suite carries over.
 */
export class SqliteStorage : public IStorage {
public:
    explicit SqliteStorage(std::shared_ptr<ISqlDatabase> database)
        : m_db(std::move(database)) {}

    /** Applies pending schema migrations and loads the id counter; closes the database when that fails. */
    Result<void> open() {
        auto migrated = SqliteMigrations().apply(*m_db);
        Result<std::optional<SqlRow>> metadata = std::optional<SqlRow>();
        if (migrated) {
            metadata = m_db->get("SELECT next_id, next_seq FROM durable_metadata WHERE singleton = 1", {});
        }
        if (!migrated || !metadata || !*metadata) {
            const Error error = !migrated ? migrated.error()
                                : !metadata ? metadata.error()
                                            : failure("Durable SQLite metadata is missing");
            (void)m_db->close();
            return std::unexpected(error);
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_nextId = std::strtoll(text(**metadata, "next_id").c_str(), nullptr, 10);
        return {};
    }

    Result<std::int64_t> commit(const std::vector<Json>& writes) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        const std::int64_t candidate = candidateNextId(writes);
        std::int64_t seq = 0;
        auto committed = m_db->transaction([&]() -> Result<void> {
            auto metadata = m_db->get("SELECT next_id, next_seq FROM durable_metadata WHERE singleton = 1", {});
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            if (!*metadata) {
                return std::unexpected(failure("Durable SQLite metadata is missing"));
            }
            seq = integer(**metadata, "next_seq");
            std::vector<Json> resolved = writes;
            if (auto copies = resolveDocumentCopies(resolved); !copies) {
                return copies;
            }
            if (auto ids = checkGlobalIds(resolved); !ids) {
                return ids;
            }
            auto actions = prepareDocumentActions(resolved);
            if (!actions) {
                return std::unexpected(actions.error());
            }
            if (auto checked = checkDocumentActions(*actions); !checked) {
                return checked;
            }
            for (const Json& write : resolved) {
                if (auto applied = applyTableWrite(write, seq); !applied) {
                    return applied;
                }
            }
            if (auto applied = applyDocumentActions(*actions, seq); !applied) {
                return applied;
            }
            const std::int64_t next = std::max<std::int64_t>(std::strtoll(text(**metadata, "next_id").c_str(), nullptr, 10), candidate);
            return m_db->run("UPDATE durable_metadata SET next_id = ?, next_seq = ? WHERE singleton = 1",
                             {std::to_string(next), seq + 1});
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_nextId = std::max(m_nextId, candidate);
        return seq;
    }

    Result<std::int64_t> mintId() override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_nextId > kMaxSafeInteger) {
            return std::unexpected(failure("ID space is exhausted"));
        }
        return m_nextId++;
    }

    Result<std::optional<Json>> conversation(std::int64_t id) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return recordById("SELECT record FROM conversations WHERE id = ?", id);
    }

    Result<StoragePage> scanConversations(const ConversationQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        auto after = scanStart(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        std::string clauses = "id > ?";
        std::vector<SqlValue> params{*after};
        if (query.ownerConversationId) {
            clauses += " AND owner_conversation_id = ?";
            params.emplace_back(*query.ownerConversationId);
        }
        if (query.ownerTaskId) {
            clauses += " AND owner_task_id = ?";
            params.emplace_back(*query.ownerTaskId);
        }
        return scanRecords("conversations", clauses, std::move(params), limit);
    }

    Result<std::optional<EntryLookup>> entry(std::int64_t id) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return readEntry(std::nullopt, id);
    }

    Result<std::optional<EntryLookup>> visibleEntry(std::int64_t conversationId, std::int64_t id) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return readEntry(conversationId, id);
    }

    Result<std::optional<Json>> findLatestHeadMarker(std::int64_t conversationId, const std::optional<std::int64_t>& atOrBeforeEntryId) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        auto current = recordById("SELECT record FROM conversations WHERE id = ?", conversationId);
        if (!current) {
            return std::unexpected(current.error());
        }
        if (!*current) {
            return std::unexpected(unknownConversation(conversationId));
        }
        Json conversationRecord = **current;
        std::optional<std::int64_t> upper = atOrBeforeEntryId;
        while (true) {
            const std::int64_t id = conversationRecord.at("id").get<std::int64_t>();
            auto marker = upper ? recordByParams("SELECT record FROM entries WHERE conversation_id = ? AND head IS NOT NULL AND id <= ? ORDER BY id DESC LIMIT 1", {id, *upper})
                                : recordByParams("SELECT record FROM entries WHERE conversation_id = ? AND head IS NOT NULL ORDER BY id DESC LIMIT 1", {id});
            if (!marker) {
                return std::unexpected(marker.error());
            }
            if (*marker) {
                return marker;
            }
            auto parent = parentOf(conversationRecord);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            if (!*parent) {
                return std::optional<Json>();
            }
            upper = upper ? std::min(*upper, (*parent)->at) : (*parent)->at;
            conversationRecord = (*parent)->record;
        }
    }

    Result<StoragePage> scanEntries(const EntryQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        auto start = recordById("SELECT record FROM conversations WHERE id = ?", query.conversationId);
        if (!start) {
            return std::unexpected(start.error());
        }
        if (!*start) {
            return std::unexpected(unknownConversation(query.conversationId));
        }
        Json conversationRecord = **start;
        std::optional<std::int64_t> upper = query.maxEntryId;
        if (*after) {
            upper = std::min(upper.value_or(kMaxSafeInteger), **after - 1);
        }
        std::vector<Json> values;
        while (true) {
            std::string clauses = "conversation_id = ?";
            std::vector<SqlValue> params{conversationRecord.at("id").get<std::int64_t>()};
            if (query.minEntryId) {
                clauses += " AND id >= ?";
                params.emplace_back(*query.minEntryId);
            }
            if (upper) {
                clauses += " AND id <= ?";
                params.emplace_back(*upper);
            }
            params.emplace_back(static_cast<std::int64_t>(limit + 1 - values.size()));
            auto rows = m_db->all("SELECT record FROM entries WHERE " + clauses + " ORDER BY id DESC LIMIT ?", params);
            if (!rows) {
                return std::unexpected(rows.error());
            }
            for (const SqlRow& row : *rows) {
                auto parsed = parse(text(row, "record"));
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                values.push_back(std::move(*parsed));
            }
            auto parent = parentOf(conversationRecord);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            if (values.size() > limit || !*parent) {
                break;
            }
            upper = upper ? std::min(*upper, (*parent)->at) : (*parent)->at;
            if (query.minEntryId && *upper < *query.minEntryId) {
                break;
            }
            conversationRecord = (*parent)->record;
        }
        return page(std::move(values), limit);
    }

    Result<std::optional<Json>> task(std::int64_t id) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return recordById("SELECT record FROM tasks WHERE id = ?", id);
    }

    Result<StoragePage> scanTasks(const TaskQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        auto after = scanStart(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        std::string clauses = "id > ?";
        std::vector<SqlValue> params{*after};
        if (query.conversationId) {
            clauses += " AND conversation_id = ?";
            params.emplace_back(*query.conversationId);
        }
        if (query.kind) {
            clauses += " AND kind = ?";
            params.emplace_back(Json(*query.kind).dump());
        }
        if (query.status) {
            clauses += " AND status = ?";
            params.emplace_back(*query.status);
        }
        if (query.abortRequested) {
            clauses += " AND abort_requested = ?";
            params.emplace_back(static_cast<std::int64_t>(*query.abortRequested ? 1 : 0));
        }
        if (query.background) {
            clauses += " AND background = ?";
            params.emplace_back(static_cast<std::int64_t>(*query.background ? 1 : 0));
        }
        return scanRecords("tasks", clauses, std::move(params), limit);
    }

    Result<std::optional<Json>> submission(std::int64_t id) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return recordById("SELECT record FROM submissions WHERE id = ?", id);
    }

    Result<StoragePage> scanSubmissions(const SubmissionQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        auto after = scanStart(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        std::string clauses = "id > ?";
        std::vector<SqlValue> params{*after};
        if (query.conversationId) {
            clauses += " AND conversation_id = ?";
            params.emplace_back(*query.conversationId);
        }
        if (query.status) {
            clauses += " AND status = ?";
            params.emplace_back(*query.status);
        }
        return scanRecords("submissions", clauses, std::move(params), limit);
    }

    Result<std::optional<Json>> submissionByRequest(std::int64_t conversationId, const std::string& requestId) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return recordByParams("SELECT record FROM submissions WHERE conversation_id = ? AND request_id = ?",
                              {conversationId, Json(requestId).dump()});
    }

    Result<std::optional<Json>> findDocument(const DocumentAddress& address, const DocumentPoint& at) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        Json named = Json::object({{"kind", address.kind}, {"scope", address.scope}});
        if (address.key) {
            named["key"] = *address.key;
        }
        std::vector<SqlValue> params = addressParams(named);
        const std::string where = "WHERE kind = ? AND scope_kind = ? AND owner_id = ? AND family = ? AND key_value = ? AND ";
        if (at.current) {
            return recordByParams("SELECT record FROM documents " + where + "retired_at IS NULL ORDER BY created_at DESC LIMIT 1", params);
        }
        params.emplace_back(at.seq);
        params.emplace_back(at.seq);
        return recordByParams("SELECT record FROM documents " + where +
                                  "created_at <= ? AND (retired_at IS NULL OR retired_at > ?) ORDER BY created_at DESC LIMIT 1",
                              params);
    }

    Result<std::optional<StoredDocument>> document(std::int64_t id, const DocumentPoint& at) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        // The record and revision queries must observe one committed state; a commit between them can replace the base.
        std::optional<StoredDocument> stored;
        auto read = m_db->transaction([&]() -> Result<void> {
            auto materialized = materializeDocument(id, at);
            if (!materialized) {
                return std::unexpected(materialized.error());
            }
            stored = std::move(*materialized);
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return stored;
    }

    Result<StoragePage> scanDocuments(const DocumentQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        auto after = scanStart(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        const auto [scopeKind, ownerId] = scopeColumns(query.scope);
        std::string clauses = "scope_kind = ? AND owner_id = ? AND id > ?";
        std::vector<SqlValue> params{scopeKind, ownerId, *after};
        if (query.kind) {
            clauses += " AND kind = ?";
            params.emplace_back(Json(*query.kind).dump());
        }
        if (query.at.current) {
            clauses += " AND retired_at IS NULL";
        } else {
            clauses += " AND created_at <= ? AND (retired_at IS NULL OR retired_at > ?)";
            params.emplace_back(query.at.seq);
            params.emplace_back(query.at.seq);
        }
        return scanRecords("documents", clauses, std::move(params), limit);
    }

    Result<void> close() override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        return m_db->close();
    }

private:
    static constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;

    Error failure(const std::string& message) const {
        return Error{"storage_error", message};
    }

    Error rejected(const std::string& message) const {
        return Error{"storage_rejected", message};
    }

    Error unknownConversation(std::int64_t id) const {
        return failure("Unknown conversation: " + std::to_string(id));
    }

    Result<void> assertOpen() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(failure("SqliteStorage is closed"));
        }
        return {};
    }

    std::int64_t integer(const SqlRow& row, const std::string& column) const {
        return std::get<std::int64_t>(row.at(column));
    }

    std::string text(const SqlRow& row, const std::string& column) const {
        return std::get<std::string>(row.at(column));
    }

    Result<Json> parse(const std::string& encoded) const {
        Json value = Json::parse(encoded, nullptr, false);
        if (value.is_discarded()) {
            return std::unexpected(failure("Stored record is not valid JSON"));
        }
        return value;
    }

    // ─── Reads ──────────────────────────────────────────────────────────────

    Result<std::optional<Json>> recordByParams(const std::string& sql, const std::vector<SqlValue>& params) {
        auto row = m_db->get(sql, params);
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return std::optional<Json>();
        }
        auto parsed = parse(text(**row, "record"));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return std::optional<Json>(std::move(*parsed));
    }

    Result<std::optional<Json>> recordById(const std::string& sql, std::int64_t id) {
        return recordByParams(sql, {id});
    }

    /** The id after which a scan resumes: the cursor's, or -1 for the start. */
    Result<std::int64_t> scanStart(const std::optional<Json>& cursor) const {
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        return after->value_or(-1);
    }

    Result<std::optional<std::int64_t>> cursorAfter(const std::optional<Json>& cursor) const {
        if (!cursor || !cursor->is_object() || !cursor->contains("after")) {
            return std::optional<std::int64_t>();
        }
        const Json& after = cursor->at("after");
        if (!after.is_number_integer()) {
            return std::unexpected(failure("Invalid storage cursor"));
        }
        return std::optional<std::int64_t>(after.get<std::int64_t>());
    }

    StoragePage page(std::vector<Json> values, std::size_t limit) const {
        StoragePage result;
        if (values.size() > limit) {
            values.resize(limit);
            result.next = Json{{"after", values.back().at("id").get<std::int64_t>()}};
        }
        result.items = std::move(values);
        return result;
    }

    /** One id-ordered page of `table` rows matching `clauses`; `params` ends before the limit binding. */
    Result<StoragePage> scanRecords(const std::string& table, const std::string& clauses, std::vector<SqlValue> params, std::size_t limit) {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        params.emplace_back(static_cast<std::int64_t>(limit + 1));
        auto rows = m_db->all("SELECT record FROM " + table + " WHERE " + clauses + " ORDER BY id LIMIT ?", params);
        if (!rows) {
            return std::unexpected(rows.error());
        }
        std::vector<Json> values;
        for (const SqlRow& row : *rows) {
            auto parsed = parse(text(row, "record"));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            values.push_back(std::move(*parsed));
        }
        return page(std::move(values), limit);
    }

    /** The parent conversation record and the entry bound it was forked at; nothing for a root conversation. */
    Result<std::optional<ForkParent>> parentOf(const Json& conversationRecord) {
        if (!conversationRecord.contains("parent")) {
            return std::optional<ForkParent>();
        }
        const Json& parent = conversationRecord.at("parent");
        auto record = recordById("SELECT record FROM conversations WHERE id = ?", parent.at("conversationId").get<std::int64_t>());
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!*record) {
            return std::unexpected(failure("Unknown conversation: " + std::to_string(parent.at("conversationId").get<std::int64_t>())));
        }
        return std::optional<ForkParent>(ForkParent{std::move(**record), parent.at("at").get<std::int64_t>()});
    }

    Result<std::optional<EntryLookup>> readEntry(const std::optional<std::int64_t>& conversationId, std::int64_t id) {
        std::optional<Json> conversationRecord;
        if (conversationId) {
            auto found = recordById("SELECT record FROM conversations WHERE id = ?", *conversationId);
            if (!found) {
                return std::unexpected(found.error());
            }
            if (!*found) {
                return std::unexpected(unknownConversation(*conversationId));
            }
            conversationRecord = **found;
        }
        auto row = m_db->get("SELECT record, commit_seq FROM entries WHERE id = ?", {id});
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!*row) {
            return std::optional<EntryLookup>();
        }
        auto entry = parse(text(**row, "record"));
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (conversationRecord) {
            std::int64_t upper = kMaxSafeInteger;
            const std::int64_t owner = entry->at("conversationId").get<std::int64_t>();
            while (conversationRecord->at("id").get<std::int64_t>() != owner) {
                auto parent = parentOf(*conversationRecord);
                if (!parent) {
                    return std::unexpected(parent.error());
                }
                if (!*parent) {
                    return std::optional<EntryLookup>();
                }
                upper = std::min(upper, (*parent)->at);
                conversationRecord = (*parent)->record;
            }
            if (id > upper) {
                return std::optional<EntryLookup>();
            }
        }
        return std::optional<EntryLookup>(EntryLookup{std::move(*entry), integer(**row, "commit_seq")});
    }

    Result<std::optional<StoredDocument>> materializeDocument(std::int64_t id, const DocumentPoint& at) {
        auto found = recordById("SELECT record FROM documents WHERE id = ?", id);
        if (!found) {
            return std::unexpected(found.error());
        }
        if (!*found) {
            return std::optional<StoredDocument>();
        }
        const Json record = **found;
        if (!at.current && currentOnly(record)) {
            return std::unexpected(failure("Document " + std::to_string(id) + " does not retain historical content"));
        }
        if (!aliveAt(record, at)) {
            return std::optional<StoredDocument>();
        }
        const std::int64_t upper = at.current ? kMaxSafeInteger : at.seq;
        auto base = m_db->get("SELECT seq, kind, version, content FROM document_revisions "
                              "WHERE document_id = ? AND kind = 'base' AND seq <= ? ORDER BY seq DESC LIMIT 1",
                              {id, upper});
        if (!base) {
            return std::unexpected(base.error());
        }
        if (!*base) {
            return std::unexpected(failure("Document " + std::to_string(id) + " is missing a required base"));
        }
        auto value = parse(text(**base, "content"));
        if (!value) {
            return std::unexpected(value.error());
        }
        auto tail = m_db->all("SELECT seq, kind, version, content FROM document_revisions WHERE document_id = ? AND seq > ? AND seq <= ? ORDER BY seq",
                              {id, integer(**base, "seq"), upper});
        if (!tail) {
            return std::unexpected(tail.error());
        }
        const DeltaApplier applier;
        for (const SqlRow& revision : *tail) {
            if (text(revision, "kind") != "delta" || integer(revision, "version") != integer(**base, "version")) {
                return std::unexpected(failure("Document " + std::to_string(id) + " crosses a stored version boundary without a base"));
            }
            auto ops = parse(text(revision, "content"));
            if (!ops) {
                return std::unexpected(ops.error());
            }
            auto applied = applier.apply(std::move(*value), *ops);
            if (!applied) {
                return std::unexpected(failure(applied.error().message));
            }
            value = std::move(*applied);
        }
        return std::optional<StoredDocument>(StoredDocument{record, integer(**base, "version"), std::move(*value), static_cast<std::int64_t>(tail->size())});
    }

    // ─── Commit checks ──────────────────────────────────────────────────────

    std::int64_t candidateNextId(const std::vector<Json>& writes) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::int64_t next = m_nextId;
        for (const Json& write : writes) {
            const std::string type = write.at("type").get<std::string>();
            if (type == "document.change" || type == "document.retire") {
                continue;
            }
            const Json& record = type == "document.create" || type == "document.copy" ? write.at("record") : write.at("value");
            next = std::max(next, record.at("id").get<std::int64_t>() + 1);
        }
        return next;
    }

    /** Replaces each `document.copy` by a creation holding the source's content as of the copy point. */
    Result<void> resolveDocumentCopies(std::vector<Json>& writes) {
        std::set<std::int64_t> changed;
        bool hasCopy = false;
        for (const Json& write : writes) {
            const std::string type = write.at("type").get<std::string>();
            hasCopy = hasCopy || type == "document.copy";
            if (type == "document.create" || type == "document.copy") {
                changed.insert(write.at("record").at("id").get<std::int64_t>());
            } else if (type == "document.change" || type == "document.retire") {
                changed.insert(write.at("id").get<std::int64_t>());
            }
        }
        if (!hasCopy) {
            return {};
        }
        for (Json& write : writes) {
            if (write.at("type") != "document.copy") {
                continue;
            }
            auto copied = copyOf(write, changed);
            if (!copied) {
                return std::unexpected(copied.error());
            }
            write = std::move(*copied);
        }
        return {};
    }

    Result<Json> copyOf(const Json& write, const std::set<std::int64_t>& changed) {
        const Json& record = write.at("record");
        const std::string what = "Document copy " + std::to_string(record.at("id").get<std::int64_t>()) + " was rejected";
        const std::int64_t sourceId = write.at("source").at("id").get<std::int64_t>();
        const std::string sourceName = "Fork source document " + std::to_string(sourceId);
        if (changed.contains(sourceId)) {
            return std::unexpected(rejected(what + ": " + sourceName + " is changed in the copy batch"));
        }
        DocumentPoint point;
        const Json& sourceAt = write.at("source").at("at");
        if (sourceAt.is_number_integer()) {
            point = DocumentPoint{false, sourceAt.get<std::int64_t>()};
        }
        auto stored = materializeDocument(sourceId, point);
        if (!stored) {
            return std::unexpected(rejected(what + ": " + stored.error().message));
        }
        if (!*stored) {
            return std::unexpected(rejected(what + ": " + sourceName + " cannot be read"));
        }
        const Json& source = (*stored)->record;
        if (source.at("scope").at("kind") != "conversation" || record.at("scope").at("kind") != "conversation" ||
            source.at("kind") != record.at("kind") || source.value("key", Json()) != record.value("key", Json()) ||
            source.at("history") != record.at("history") || source.at("fork") != record.at("fork")) {
            return std::unexpected(rejected(what + ": " + sourceName + " does not match the copied record"));
        }
        return Json{{"type", "document.create"},
                    {"record", record},
                    {"content", Json{{"kind", "base"}, {"version", (*stored)->version}, {"value", (*stored)->value}}}};
    }

    Result<void> checkGlobalIds(const std::vector<Json>& writes) {
        std::map<std::int64_t, std::string> claimed;
        for (const Json& write : writes) {
            const std::string type = write.at("type").get<std::string>();
            if (type == "document.change" || type == "document.retire") {
                continue;
            }
            const bool document = type == "document.create" || type == "document.copy";
            const std::string table = document ? "document" : type;
            const std::int64_t id = (document ? write.at("record") : write.at("value")).at("id").get<std::int64_t>();
            auto row = m_db->get("SELECT record_type FROM record_ids WHERE id = ?", {id});
            if (!row) {
                return std::unexpected(row.error());
            }
            const std::optional<std::string> existing = *row ? std::optional<std::string>(text(**row, "record_type")) : std::nullopt;
            const auto earlier = claimed.find(id);
            const std::string name = std::to_string(id);
            if (table == "conversation" || table == "entry" || table == "document") {
                if (existing) {
                    return std::unexpected(failure("ID " + name + " already belongs to " + *existing));
                }
                if (earlier != claimed.end()) {
                    return std::unexpected(failure("ID " + name + " is written more than once"));
                }
            } else {
                if (existing && *existing != table) {
                    return std::unexpected(failure("ID " + name + " already belongs to " + *existing));
                }
                if (earlier != claimed.end() && earlier->second != table) {
                    return std::unexpected(failure("ID " + name + " is written as two record types"));
                }
            }
            claimed[id] = table;
        }
        return {};
    }

    Result<std::map<std::int64_t, DocumentAction>> prepareDocumentActions(const std::vector<Json>& writes) const {
        std::map<std::int64_t, DocumentAction> actions;
        for (const Json& write : writes) {
            const std::string type = write.at("type").get<std::string>();
            if (type != "document.create" && type != "document.change" && type != "document.retire") {
                continue;
            }
            const std::int64_t id = type == "document.create" ? write.at("record").at("id").get<std::int64_t>() : write.at("id").get<std::int64_t>();
            DocumentAction& action = actions[id];
            const std::string name = std::to_string(id);
            if (type == "document.create") {
                if (action.create || action.content) {
                    return std::unexpected(failure("Document " + name + " has more than one content command"));
                }
                action.create = write.at("record");
                action.content = write.at("content");
            } else if (type == "document.change") {
                if (action.content) {
                    return std::unexpected(failure("Document " + name + " has more than one content command"));
                }
                action.content = write.at("content");
            } else {
                if (action.retire) {
                    return std::unexpected(failure("Document " + name + " is retired more than once"));
                }
                action.retire = true;
            }
        }
        return actions;
    }

    Result<void> checkDocumentActions(const std::map<std::int64_t, DocumentAction>& actions) {
        std::map<std::string, int> liveCounts;
        for (const auto& [id, action] : actions) {
            const std::string name = std::to_string(id);
            auto existing = recordById("SELECT record FROM documents WHERE id = ?", id);
            if (!existing) {
                return std::unexpected(existing.error());
            }
            if (!action.create && !*existing) {
                return std::unexpected(failure("Unknown document: " + name));
            }
            if (action.create && *existing) {
                return std::unexpected(failure("Document " + name + " already exists"));
            }
            if (*existing && (*existing)->contains("retiredAt")) {
                return std::unexpected(failure("Document " + name + " is retired"));
            }
            if (action.content && action.content->at("kind") == "delta") {
                auto previous = m_db->get("SELECT version FROM document_revisions WHERE document_id = ? ORDER BY seq DESC LIMIT 1", {id});
                if (!previous) {
                    return std::unexpected(previous.error());
                }
                if (!*previous) {
                    return std::unexpected(failure("Document " + name + " delta has no base"));
                }
                if (integer(**previous, "version") != action.content->at("version").get<std::int64_t>()) {
                    return std::unexpected(failure("Document " + name + " version transition requires a base"));
                }
            }
            const Json& record = action.create ? *action.create : **existing;
            const std::string key = Json(addressJson(addressParams(record))).dump();
            auto live = liveCounts.find(key);
            int count = 0;
            if (live != liveCounts.end()) {
                count = live->second;
            } else {
                auto current = currentDocumentId(record);
                if (!current) {
                    return std::unexpected(current.error());
                }
                count = *current ? 1 : 0;
            }
            if (action.retire && *existing) {
                --count;
            }
            if (action.create && !action.retire) {
                ++count;
            }
            liveCounts[key] = count;
        }
        for (const auto& entry : liveCounts) {
            if (entry.second > 1) {
                return std::unexpected(failure("Document address already has a current incarnation"));
            }
        }
        return {};
    }

    Result<std::optional<std::int64_t>> currentDocumentId(const Json& address) {
        auto row = m_db->get("SELECT id FROM documents WHERE kind = ? AND scope_kind = ? AND owner_id = ? AND family = ? AND key_value = ? "
                             "AND retired_at IS NULL LIMIT 1",
                             addressParams(address));
        if (!row) {
            return std::unexpected(row.error());
        }
        return *row ? std::optional<std::int64_t>(integer(**row, "id")) : std::nullopt;
    }

    // ─── Commit application ─────────────────────────────────────────────────

    Result<void> applyTableWrite(const Json& write, std::int64_t seq) {
        const std::string type = write.at("type").get<std::string>();
        if (type == "conversation") {
            const Json& value = write.at("value");
            const bool owned = value.contains("owner");
            return claimAndRun(value, "conversation",
                               "INSERT INTO conversations (id, owner_conversation_id, owner_task_id, record) VALUES (?, ?, ?, ?)",
                               {id(value), owned ? SqlValue(value.at("owner").at("conversationId").get<std::int64_t>()) : SqlValue(std::monostate{}),
                                owned ? SqlValue(value.at("owner").at("taskId").get<std::int64_t>()) : SqlValue(std::monostate{}), value.dump()});
        }
        if (type == "entry") {
            const Json& value = write.at("value");
            return claimAndRun(value, "entry", "INSERT INTO entries (id, conversation_id, head, commit_seq, record) VALUES (?, ?, ?, ?, ?)",
                               {id(value), value.at("conversationId").get<std::int64_t>(),
                                value.contains("head") ? SqlValue(value.at("head").get<std::int64_t>()) : SqlValue(std::monostate{}), seq, value.dump()});
        }
        if (type == "task") {
            const Json& value = write.at("value");
            return claimAndRun(value, "task",
                               "INSERT INTO tasks (id, conversation_id, kind, status, abort_requested, background, record) VALUES (?, ?, ?, ?, ?, ?, ?) "
                               "ON CONFLICT(id) DO UPDATE SET conversation_id = excluded.conversation_id, kind = excluded.kind, "
                               "status = excluded.status, abort_requested = excluded.abort_requested, "
                               "background = excluded.background, record = excluded.record",
                               {id(value), value.at("conversationId").get<std::int64_t>(), Json(value.at("kind")).dump(),
                                value.at("state").at("status").get<std::string>(),
                                static_cast<std::int64_t>(value.value("abortRequested", false) ? 1 : 0),
                                static_cast<std::int64_t>(value.value("background", false) ? 1 : 0), value.dump()});
        }
        if (type == "submission") {
            const Json& value = write.at("value");
            return claimAndRun(value, "submission",
                               "INSERT INTO submissions (id, conversation_id, request_id, status, record) VALUES (?, ?, ?, ?, ?) "
                               "ON CONFLICT(id) DO UPDATE SET conversation_id = excluded.conversation_id, "
                               "request_id = excluded.request_id, status = excluded.status, record = excluded.record",
                               {id(value), value.at("conversationId").get<std::int64_t>(),
                                value.contains("requestId") ? SqlValue(Json(value.at("requestId")).dump()) : SqlValue(std::monostate{}),
                                value.at("status").get<std::string>(), value.dump()});
        }
        return {};
    }

    std::int64_t id(const Json& record) const {
        return record.at("id").get<std::int64_t>();
    }

    Result<void> claimAndRun(const Json& value, const std::string& table, const std::string& sql, const std::vector<SqlValue>& params) {
        if (auto claimed = claimId(id(value), table); !claimed) {
            return claimed;
        }
        return m_db->run(sql, params);
    }

    Result<void> claimId(std::int64_t recordId, const std::string& table) {
        return m_db->run("INSERT OR IGNORE INTO record_ids (id, record_type) VALUES (?, ?)", {recordId, table});
    }

    Result<void> applyDocumentActions(const std::map<std::int64_t, DocumentAction>& actions, std::int64_t seq) {
        for (const auto& [documentId, action] : actions) {
            Json record;
            if (action.create) {
                record = *action.create;
                record["createdAt"] = seq;
                if (action.retire) {
                    record["retiredAt"] = seq;
                }
                const std::vector<SqlValue> parts = addressParams(record);
                if (auto claimed = claimId(documentId, "document"); !claimed) {
                    return claimed;
                }
                if (auto inserted = m_db->run("INSERT INTO documents (id, kind, family, key_value, scope_kind, owner_id, created_at, retired_at, record) "
                                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                              {documentId, parts[0], parts[3], parts[4], parts[1], parts[2], seq,
                                               action.retire ? SqlValue(seq) : SqlValue(std::monostate{}), record.dump()});
                    !inserted) {
                    return inserted;
                }
            } else {
                auto existing = recordById("SELECT record FROM documents WHERE id = ?", documentId);
                if (!existing) {
                    return std::unexpected(existing.error());
                }
                record = **existing;
            }
            if (action.content) {
                if (auto stored = storeRevision(documentId, record, *action.content, seq); !stored) {
                    return stored;
                }
            }
            if (action.retire) {
                if (auto retired = retire(documentId, record, !action.create, seq); !retired) {
                    return retired;
                }
            }
        }
        return {};
    }

    Result<void> storeRevision(std::int64_t documentId, const Json& record, const Json& content, std::int64_t seq) {
        const bool base = content.at("kind") == "base";
        if (base && currentOnly(record)) {
            if (auto cleared = m_db->run("DELETE FROM document_revisions WHERE document_id = ?", {documentId}); !cleared) {
                return cleared;
            }
        }
        return m_db->run("INSERT INTO document_revisions (document_id, seq, kind, version, content) VALUES (?, ?, ?, ?, ?)",
                         {documentId, seq, content.at("kind").get<std::string>(), content.at("version").get<std::int64_t>(),
                          (base ? content.at("value") : content.at("ops")).dump()});
    }

    Result<void> retire(std::int64_t documentId, Json record, bool existing, std::int64_t seq) {
        if (existing) {
            record["retiredAt"] = seq;
            if (auto updated = m_db->run("UPDATE documents SET retired_at = ?, record = ? WHERE id = ?", {seq, record.dump(), documentId}); !updated) {
                return updated;
            }
        }
        if (currentOnly(record)) {
            return m_db->run("DELETE FROM document_revisions WHERE document_id = ?", {documentId});
        }
        return {};
    }

    // ─── Document addressing ────────────────────────────────────────────────

    std::pair<std::string, std::int64_t> scopeColumns(const Json& scope) const {
        const std::string kind = scope.at("kind").get<std::string>();
        if (kind == "conversation") {
            return {kind, scope.at("conversationId").get<std::int64_t>()};
        }
        if (kind == "task") {
            return {kind, scope.at("taskId").get<std::int64_t>()};
        }
        return {kind, 0};
    }

    /** `[kind, scope_kind, owner_id, family, key_value]` of an address-like record; identity strings are JSON-encoded. */
    std::vector<SqlValue> addressParams(const Json& address) const {
        const auto [scopeKind, ownerId] = scopeColumns(address.at("scope"));
        const bool family = address.contains("key");
        return {Json(address.at("kind")).dump(), scopeKind, ownerId, static_cast<std::int64_t>(family ? 1 : 0),
                Json(family ? address.at("key") : Json("")).dump()};
    }

    Json addressJson(const std::vector<SqlValue>& parts) const {
        Json out = Json::array();
        for (const SqlValue& part : parts) {
            if (std::holds_alternative<std::int64_t>(part)) {
                out.push_back(std::get<std::int64_t>(part));
            } else {
                out.push_back(std::get<std::string>(part));
            }
        }
        return out;
    }

    bool aliveAt(const Json& record, const DocumentPoint& at) const {
        const bool retired = record.contains("retiredAt");
        if (at.current) {
            return !retired;
        }
        return record.at("createdAt").get<std::int64_t>() <= at.seq && (!retired || at.seq < record.at("retiredAt").get<std::int64_t>());
    }

    bool currentOnly(const Json& record) const {
        return record.at("scope").at("kind") != "conversation" || record.at("history") == "latest";
    }

    std::shared_ptr<ISqlDatabase> m_db;
    std::mutex m_mutex;
    std::int64_t m_nextId = 2;
    bool m_closed = false;
};
