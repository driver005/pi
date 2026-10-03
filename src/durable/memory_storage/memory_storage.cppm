module;

#include <cstdint>

export module pi.durable.memory_storage;

import std;
export import pi.durable.i_staged_storage;
export import pi.types.document_action;
import pi.support.delta_applier;

/**
 * In-memory reference implementation of IStorage. Every record is copied on the way in and out, so
 * callers never share state with the store (the ownership boundary of serialization-backed stores).
 * Ordered containers stand in for the sorted id arrays of the TS original. Thread-safe. Port of
 * packages/durable/src/storage/memory.ts; error messages match it so the conformance suite carries over.
 */
export class MemoryStorage : public IStagedStorage {
public:
    Result<std::int64_t> commit(const std::vector<Json>& writes) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto prepared = prepareLocked(writes, std::nullopt);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        return applyLocked(*prepared);
    }

    Result<PreparedCommit> prepareCommit(const std::vector<Json>& writes, const std::optional<std::int64_t>& seq) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return prepareLocked(writes, seq);
    }

    Result<std::int64_t> applyCommit(const PreparedCommit& commit) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return applyLocked(commit);
    }

    Result<std::int64_t> mintId() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        if (m_nextId > kMaxSafeInteger) {
            return std::unexpected(failure("ID space is exhausted"));
        }
        return m_nextId++;
    }

    Result<std::optional<Json>> conversation(std::int64_t id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return find(m_conversations, id);
    }

    Result<StoragePage> scanConversations(const ConversationQuery& query, std::size_t limit,
                                          const std::optional<Json>& cursor) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        const std::set<std::int64_t>* ids = &m_conversationIds;
        if (query.ownerTaskId) {
            ids = indexed(m_conversationsByOwnerTask, *query.ownerTaskId);
        } else if (query.ownerConversationId) {
            ids = indexed(m_conversationsByOwnerConversation, *query.ownerConversationId);
        }
        return scan(*ids, m_conversations, *after, limit, [&](const Json& value) {
            if (!query.ownerConversationId) {
                return true;
            }
            return value.contains("owner") && value["owner"]["conversationId"] == *query.ownerConversationId;
        });
    }

    Result<std::optional<EntryLookup>> entry(std::int64_t id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return lookup(id);
    }

    Result<std::optional<EntryLookup>> visibleEntry(std::int64_t conversationId, std::int64_t id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto visible = visibleEntries(conversationId, id, id, 1);
        if (!visible) {
            return std::unexpected(visible.error());
        }
        if (visible->empty()) {
            return std::optional<EntryLookup>();
        }
        return lookup(id);
    }

    Result<std::optional<Json>> findLatestHeadMarker(std::int64_t conversationId,
                                                     const std::optional<std::int64_t>& atOrBeforeEntryId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        if (!m_conversations.contains(conversationId)) {
            return std::unexpected(unknownConversation(conversationId));
        }
        std::int64_t current = conversationId;
        std::int64_t upper = atOrBeforeEntryId.value_or(std::numeric_limits<std::int64_t>::max());
        while (true) {
            const auto heads = m_headEntryIds.find(current);
            if (heads != m_headEntryIds.end()) {
                auto it = heads->second.upper_bound(upper);
                if (it != heads->second.begin()) {
                    return std::optional<Json>(m_entries.at(*std::prev(it)));
                }
            }
            const Json& conversationRecord = m_conversations.at(current);
            if (!conversationRecord.contains("parent")) {
                return std::optional<Json>();
            }
            upper = std::min<std::int64_t>(upper, conversationRecord["parent"]["at"].get<std::int64_t>());
            current = conversationRecord["parent"]["conversationId"].get<std::int64_t>();
        }
    }

    Result<StoragePage> scanEntries(const EntryQuery& query, std::size_t limit,
                                    const std::optional<Json>& cursor) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        std::optional<std::int64_t> maxEntryId = query.maxEntryId;
        if (*after) {
            maxEntryId = std::min(maxEntryId.value_or(std::numeric_limits<std::int64_t>::max()), **after - 1);
        }
        auto visible = visibleEntries(query.conversationId, query.minEntryId, maxEntryId, limit + 1);
        if (!visible) {
            return std::unexpected(visible.error());
        }
        return page(std::move(*visible), limit);
    }

    Result<std::optional<Json>> task(std::int64_t id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return find(m_tasks, id);
    }

    Result<StoragePage> scanTasks(const TaskQuery& query, std::size_t limit, const std::optional<Json>& cursor) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        const std::set<std::int64_t>& ids = query.status ? *indexed(m_taskIdsByStatus, *query.status) : m_taskIds;
        return scan(ids, m_tasks, *after, limit, [&](const Json& value) {
            return (!query.conversationId || value["conversationId"] == *query.conversationId) &&
                   (!query.kind || value["kind"] == *query.kind) &&
                   (!query.abortRequested || value["abortRequested"] == *query.abortRequested) &&
                   (!query.background || value["background"] == *query.background);
        });
    }

    Result<std::optional<Json>> submission(std::int64_t id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return find(m_submissions, id);
    }

    Result<StoragePage> scanSubmissions(const SubmissionQuery& query, std::size_t limit,
                                        const std::optional<Json>& cursor) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        const std::set<std::int64_t>& ids = query.status ? *indexed(m_submissionIdsByStatus, *query.status) : m_submissionIds;
        return scan(ids, m_submissions, *after, limit, [&](const Json& value) {
            return !query.conversationId || value["conversationId"] == *query.conversationId;
        });
    }

    Result<std::optional<Json>> submissionByRequest(std::int64_t conversationId, const std::string& requestId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        const auto requests = m_submissionIdsByRequest.find(conversationId);
        if (requests == m_submissionIdsByRequest.end()) {
            return std::optional<Json>();
        }
        const auto id = requests->second.find(requestId);
        if (id == requests->second.end()) {
            return std::optional<Json>();
        }
        return std::optional<Json>(m_submissions.at(id->second));
    }

    Result<std::optional<Json>> findDocument(const DocumentAddress& address, const DocumentPoint& at) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        const std::string key = addressKey(address.kind, address.scope, address.key);
        if (at.current) {
            const auto current = m_addressCurrent.find(key);
            if (current == m_addressCurrent.end()) {
                return std::optional<Json>();
            }
            return std::optional<Json>(m_documentRecords.at(current->second));
        }
        const auto ids = m_addressIds.find(key);
        if (ids != m_addressIds.end()) {
            for (const std::int64_t id : ids->second) {
                const Json& record = m_documentRecords.at(id);
                if (aliveAt(record, at)) {
                    return std::optional<Json>(record);
                }
            }
        }
        return std::optional<Json>();
    }

    Result<std::optional<StoredDocument>> document(std::int64_t id, const DocumentPoint& at) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        return materializeDocument(id, at);
    }

    Result<StoragePage> scanDocuments(const DocumentQuery& query, std::size_t limit,
                                      const std::optional<Json>& cursor) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(closedError());
        }
        auto after = cursorAfter(cursor);
        if (!after) {
            return std::unexpected(after.error());
        }
        const std::set<std::int64_t>* ids = indexed(m_documentIdsByScope, scopeKey(query.scope));
        return scan(*ids, m_documentRecords, *after, limit, [&](const Json& record) {
            return (!query.kind || record["kind"] == *query.kind) && aliveAt(record, query.at);
        });
    }

    Result<void> close() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        return {};
    }

private:
    static constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;

    Result<PreparedCommit> prepareLocked(const std::vector<Json>& writes, const std::optional<std::int64_t>& requested) {
        if (m_closed) {
            return std::unexpected(closedError());
        }
        PreparedCommit prepared;
        prepared.seq = requested.value_or(m_nextSeq);
        if (prepared.seq < m_nextSeq) {
            return std::unexpected(failure("Commit sequence " + std::to_string(prepared.seq) + " does not strictly increase"));
        }
        prepared.writes = writes;
        if (auto resolved = resolveDocumentCopies(prepared.writes); !resolved) {
            return std::unexpected(resolved.error());
        }
        if (auto ids = checkGlobalIds(prepared.writes); !ids) {
            return std::unexpected(ids.error());
        }
        auto actions = prepareDocumentActions(prepared.writes);
        if (!actions) {
            return std::unexpected(actions.error());
        }
        if (auto checked = checkDocumentActions(*actions); !checked) {
            return std::unexpected(checked.error());
        }
        prepared.actions = std::move(*actions);
        return prepared;
    }

    std::int64_t applyLocked(const PreparedCommit& prepared) {
        applyWrites(prepared.writes, prepared.seq);
        applyDocumentActions(prepared.actions, prepared.seq);
        m_nextSeq = prepared.seq + 1;
        return prepared.seq;
    }

    Error failure(const std::string& message) const {
        return Error{"storage_error", message};
    }

    Error rejected(const std::string& message) const {
        return Error{"storage_rejected", message};
    }

    Error closedError() const {
        return failure("MemoryStorage is closed");
    }

    Error unknownConversation(std::int64_t id) const {
        return failure("Unknown conversation: " + std::to_string(id));
    }

    std::int64_t idOf(const Json& record) const {
        return record["id"].get<std::int64_t>();
    }

    Result<std::optional<Json>> find(const std::map<std::int64_t, Json>& table, std::int64_t id) const {
        const auto found = table.find(id);
        return found == table.end() ? std::optional<Json>() : std::optional<Json>(found->second);
    }

    Result<std::optional<EntryLookup>> lookup(std::int64_t id) const {
        const auto found = m_entries.find(id);
        if (found == m_entries.end()) {
            return std::optional<EntryLookup>();
        }
        return std::optional<EntryLookup>(EntryLookup{found->second, m_entryCommitSeqs.at(id)});
    }

    template <typename Key>
    const std::set<std::int64_t>* indexed(const std::map<Key, std::set<std::int64_t>>& index, const Key& key) const {
        const auto found = index.find(key);
        return found == index.end() ? &m_noIds : &found->second;
    }

    Result<std::optional<std::int64_t>> cursorAfter(const std::optional<Json>& cursor) const {
        if (!cursor || !cursor->is_object() || !cursor->contains("after")) {
            return std::optional<std::int64_t>();
        }
        const Json& after = (*cursor)["after"];
        if (!after.is_number_integer()) {
            return std::unexpected(failure("Invalid storage cursor"));
        }
        return std::optional<std::int64_t>(after.get<std::int64_t>());
    }

    StoragePage page(std::vector<Json> values, std::size_t limit) const {
        StoragePage result;
        if (values.size() > limit) {
            values.resize(limit);
            result.next = Json{{"after", idOf(values.back())}};
        }
        result.items = std::move(values);
        return result;
    }

    Result<StoragePage> scan(const std::set<std::int64_t>& ids, const std::map<std::int64_t, Json>& table,
                             const std::optional<std::int64_t>& after, std::size_t limit,
                             const std::function<bool(const Json&)>& accept) const {
        std::vector<Json> values;
        auto it = after ? ids.upper_bound(*after) : ids.begin();
        for (; it != ids.end() && values.size() <= limit; ++it) {
            const Json& value = table.at(*it);
            if (accept(value)) {
                values.push_back(value);
            }
        }
        return page(std::move(values), limit);
    }

    Result<std::vector<Json>> visibleEntries(std::int64_t conversationId, const std::optional<std::int64_t>& minEntryId,
                                             const std::optional<std::int64_t>& maxEntryId, std::size_t maxCount) const {
        if (!m_conversations.contains(conversationId)) {
            return std::unexpected(unknownConversation(conversationId));
        }
        const std::int64_t lower = minEntryId.value_or(std::numeric_limits<std::int64_t>::min());
        std::int64_t upper = maxEntryId.value_or(std::numeric_limits<std::int64_t>::max());
        std::int64_t current = conversationId;
        std::vector<Json> out;
        while (true) {
            const auto ids = m_entryIds.find(current);
            if (ids != m_entryIds.end()) {
                auto it = ids->second.upper_bound(upper);
                while (it != ids->second.begin()) {
                    --it;
                    if (*it < lower) {
                        break;
                    }
                    out.push_back(m_entries.at(*it));
                    if (out.size() >= maxCount) {
                        return out;
                    }
                }
            }
            const Json& conversationRecord = m_conversations.at(current);
            if (!conversationRecord.contains("parent")) {
                break;
            }
            upper = std::min<std::int64_t>(upper, conversationRecord["parent"]["at"].get<std::int64_t>());
            if (upper < lower) {
                break;
            }
            current = conversationRecord["parent"]["conversationId"].get<std::int64_t>();
        }
        return out;
    }

    std::string scopeKey(const Json& scope) const {
        const std::string kind = scope["kind"].get<std::string>();
        if (kind == "conversation") {
            return Json::array({kind, scope["conversationId"]}).dump();
        }
        if (kind == "task") {
            return Json::array({kind, scope["taskId"]}).dump();
        }
        return Json::array({kind}).dump();
    }

    std::string addressKey(const std::string& kind, const Json& scope, const std::optional<std::string>& key) const {
        return Json::array({kind, scopeKey(scope),
                            key ? Json::array({"family", *key}) : Json::array({"singleton"})})
            .dump();
    }

    std::string recordAddressKey(const Json& record) const {
        std::optional<std::string> key;
        if (record.contains("key")) {
            key = record["key"].get<std::string>();
        }
        return addressKey(record["kind"].get<std::string>(), record["scope"], key);
    }

    bool aliveAt(const Json& record, const DocumentPoint& at) const {
        const bool retired = record.contains("retiredAt");
        if (at.current) {
            return !retired;
        }
        return record["createdAt"].get<std::int64_t>() <= at.seq && (!retired || at.seq < record["retiredAt"].get<std::int64_t>());
    }

    bool currentOnly(const Json& record) const {
        return record["scope"]["kind"] != "conversation" || record["history"] == "latest";
    }

    Result<std::optional<StoredDocument>> materializeDocument(std::int64_t id, const DocumentPoint& at) const {
        const auto recordIt = m_documentRecords.find(id);
        if (recordIt == m_documentRecords.end()) {
            return std::optional<StoredDocument>();
        }
        const Json& record = recordIt->second;
        if (!at.current && currentOnly(record)) {
            return std::unexpected(failure("Document " + std::to_string(id) + " does not retain historical content"));
        }
        if (!aliveAt(record, at)) {
            return std::optional<StoredDocument>();
        }
        std::vector<const Json*> revisions;
        for (const Json& revision : m_documentRevisions.at(id)) {
            if (at.current || revision["seq"].get<std::int64_t>() <= at.seq) {
                revisions.push_back(&revision);
            }
        }
        std::size_t baseIndex = revisions.size();
        while (baseIndex > 0 && (*revisions[baseIndex - 1])["kind"] != "base") {
            --baseIndex;
        }
        if (baseIndex == 0) {
            return std::unexpected(failure("Document " + std::to_string(id) + " is missing a required base"));
        }
        --baseIndex;
        const Json& base = *revisions[baseIndex];
        Json value = base["value"];
        const DeltaApplier applier;
        for (std::size_t i = baseIndex + 1; i < revisions.size(); ++i) {
            const Json& revision = *revisions[i];
            if (revision["kind"] != "delta" || revision["version"] != base["version"]) {
                return std::unexpected(failure("Document " + std::to_string(id) +
                                               " crosses a stored version boundary without a base"));
            }
            auto applied = applier.apply(std::move(value), revision["ops"]);
            if (!applied) {
                return std::unexpected(failure(applied.error().message));
            }
            value = std::move(*applied);
        }
        return std::optional<StoredDocument>(StoredDocument{record, base["version"].get<std::int64_t>(), std::move(value),
                                                            static_cast<std::int64_t>(revisions.size() - baseIndex - 1)});
    }

    Result<void> resolveDocumentCopies(std::vector<Json>& writes) const {
        std::set<std::int64_t> changed;
        bool hasCopy = false;
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            hasCopy = hasCopy || type == "document.copy";
            if (type == "document.create" || type == "document.copy") {
                changed.insert(idOf(write["record"]));
            } else if (type == "document.change" || type == "document.retire") {
                changed.insert(write["id"].get<std::int64_t>());
            }
        }
        if (!hasCopy) {
            return {};
        }
        for (Json& write : writes) {
            if (write["type"] != "document.copy") {
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

    Result<Json> copyOf(const Json& write, const std::set<std::int64_t>& changed) const {
        const Json& record = write["record"];
        const std::string what = "Document copy " + std::to_string(idOf(record)) + " was rejected";
        const std::int64_t sourceId = write["source"]["id"].get<std::int64_t>();
        const std::string sourceName = "Fork source document " + std::to_string(sourceId);
        if (changed.contains(sourceId)) {
            return std::unexpected(rejected(what + ": " + sourceName + " is changed in the copy batch"));
        }
        DocumentPoint point;
        const Json& sourceAt = write["source"]["at"];
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
        if (source["scope"]["kind"] != "conversation" || record["scope"]["kind"] != "conversation" ||
            source["kind"] != record["kind"] || source.value("key", Json()) != record.value("key", Json()) ||
            source["history"] != record["history"] || source["fork"] != record["fork"]) {
            return std::unexpected(rejected(what + ": " + sourceName + " does not match the copied record"));
        }
        return Json{{"type", "document.create"},
                    {"record", record},
                    {"content", Json{{"kind", "base"}, {"version", (*stored)->version}, {"value", (*stored)->value}}}};
    }

    Result<void> checkGlobalIds(const std::vector<Json>& writes) const {
        std::map<std::int64_t, std::string> claimed;
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            if (type == "document.change" || type == "document.retire") {
                continue;
            }
            const bool document = type == "document.create" || type == "document.copy";
            const std::string table = document ? "document" : type;
            const std::int64_t id = idOf(document ? write["record"] : write["value"]);
            const auto existing = m_recordTypes.find(id);
            const auto earlier = claimed.find(id);
            const std::string name = std::to_string(id);
            if (table == "conversation" || table == "entry" || table == "document") {
                if (existing != m_recordTypes.end()) {
                    return std::unexpected(failure("ID " + name + " already belongs to " + existing->second));
                }
                if (earlier != claimed.end()) {
                    return std::unexpected(failure("ID " + name + " is written more than once"));
                }
            } else {
                if (existing != m_recordTypes.end() && existing->second != table) {
                    return std::unexpected(failure("ID " + name + " already belongs to " + existing->second));
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
            const std::string type = write["type"].get<std::string>();
            if (type != "document.create" && type != "document.change" && type != "document.retire") {
                continue;
            }
            const std::int64_t id = type == "document.create" ? idOf(write["record"]) : write["id"].get<std::int64_t>();
            DocumentAction& action = actions[id];
            const std::string name = std::to_string(id);
            if (type == "document.create") {
                if (action.create || action.content) {
                    return std::unexpected(failure("Document " + name + " has more than one content command"));
                }
                action.create = write["record"];
                action.content = write["content"];
            } else if (type == "document.change") {
                if (action.content) {
                    return std::unexpected(failure("Document " + name + " has more than one content command"));
                }
                action.content = write["content"];
            } else {
                if (action.retire) {
                    return std::unexpected(failure("Document " + name + " is retired more than once"));
                }
                action.retire = true;
            }
        }
        return actions;
    }

    Result<void> checkDocumentActions(const std::map<std::int64_t, DocumentAction>& actions) const {
        std::map<std::string, int> liveCounts;
        for (const auto& [id, action] : actions) {
            const std::string name = std::to_string(id);
            const auto existing = m_documentRecords.find(id);
            const bool exists = existing != m_documentRecords.end();
            if (!action.create && !exists) {
                return std::unexpected(failure("Unknown document: " + name));
            }
            if (action.create && exists) {
                return std::unexpected(failure("Document " + name + " already exists"));
            }
            if (exists && existing->second.contains("retiredAt")) {
                return std::unexpected(failure("Document " + name + " is retired"));
            }
            if (action.content && (*action.content)["kind"] == "delta") {
                if (!exists) {
                    return std::unexpected(failure("Document " + name + " delta has no base"));
                }
                if (m_documentRevisions.at(id).back()["version"] != (*action.content)["version"]) {
                    return std::unexpected(failure("Document " + name + " version transition requires a base"));
                }
            }
            const std::string key = recordAddressKey(action.create ? *action.create : existing->second);
            const auto current = m_addressCurrent.find(key);
            const bool hasCurrent = current != m_addressCurrent.end();
            auto live = liveCounts.find(key);
            int count = live != liveCounts.end() ? live->second : (hasCurrent ? 1 : 0);
            if (action.retire && hasCurrent && current->second == id) {
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

    void applyWrites(const std::vector<Json>& writes, std::int64_t seq) {
        for (const Json& write : writes) {
            const std::string type = write["type"].get<std::string>();
            if (type == "conversation") {
                applyConversation(write["value"]);
            } else if (type == "entry") {
                applyEntry(write["value"], seq);
            } else if (type == "task") {
                applyTask(write["value"]);
            } else if (type == "submission") {
                applySubmission(write["value"]);
            }
        }
    }

    void applyConversation(const Json& value) {
        const std::int64_t id = idOf(value);
        m_recordTypes[id] = "conversation";
        m_conversations[id] = value;
        m_conversationIds.insert(id);
        if (value.contains("owner")) {
            m_conversationsByOwnerConversation[value["owner"]["conversationId"].get<std::int64_t>()].insert(id);
            m_conversationsByOwnerTask[value["owner"]["taskId"].get<std::int64_t>()].insert(id);
        }
        m_nextId = std::max(m_nextId, id + 1);
    }

    void applyEntry(const Json& value, std::int64_t seq) {
        const std::int64_t id = idOf(value);
        const std::int64_t conversationId = value["conversationId"].get<std::int64_t>();
        m_recordTypes[id] = "entry";
        m_entries[id] = value;
        m_entryCommitSeqs[id] = seq;
        m_entryIds[conversationId].insert(id);
        if (value.contains("head")) {
            m_headEntryIds[conversationId].insert(id);
        }
        m_nextId = std::max(m_nextId, id + 1);
    }

    void applyTask(const Json& value) {
        const std::int64_t id = idOf(value);
        m_recordTypes[id] = "task";
        const auto previous = m_tasks.find(id);
        const std::string status = value["state"]["status"].get<std::string>();
        if (previous == m_tasks.end()) {
            m_taskIds.insert(id);
        } else {
            m_taskIdsByStatus[previous->second["state"]["status"].get<std::string>()].erase(id);
        }
        m_taskIdsByStatus[status].insert(id);
        m_tasks[id] = value;
        m_nextId = std::max(m_nextId, id + 1);
    }

    void applySubmission(const Json& value) {
        const std::int64_t id = idOf(value);
        m_recordTypes[id] = "submission";
        const auto previous = m_submissions.find(id);
        if (previous == m_submissions.end()) {
            m_submissionIds.insert(id);
        } else {
            const Json& old = previous->second;
            m_submissionIdsByStatus[old["status"].get<std::string>()].erase(id);
            if (old.contains("requestId")) {
                auto requests = m_submissionIdsByRequest.find(old["conversationId"].get<std::int64_t>());
                if (requests != m_submissionIdsByRequest.end()) {
                    const std::string request = old["requestId"].get<std::string>();
                    const auto owner = requests->second.find(request);
                    if (owner != requests->second.end() && owner->second == id) {
                        requests->second.erase(owner);
                    }
                    if (requests->second.empty()) {
                        m_submissionIdsByRequest.erase(requests);
                    }
                }
            }
        }
        m_submissionIdsByStatus[value["status"].get<std::string>()].insert(id);
        m_submissions[id] = value;
        if (value.contains("requestId")) {
            m_submissionIdsByRequest[value["conversationId"].get<std::int64_t>()][value["requestId"].get<std::string>()] = id;
        }
        m_nextId = std::max(m_nextId, id + 1);
    }

    void applyDocumentActions(const std::map<std::int64_t, DocumentAction>& actions, std::int64_t seq) {
        for (const auto& [id, action] : actions) {
            if (action.create) {
                createDocument(id, action, seq);
            } else if (action.content) {
                Json revision = *action.content;
                revision["seq"] = seq;
                std::vector<Json>& revisions = m_documentRevisions[id];
                if (revision["kind"] == "base" && currentOnly(m_documentRecords.at(id))) {
                    revisions.clear();
                }
                revisions.push_back(std::move(revision));
            }
            if (action.retire && !action.create) {
                m_documentRecords[id]["retiredAt"] = seq;
            }
            if (action.retire && currentOnly(m_documentRecords.at(id))) {
                m_documentRevisions[id].clear();
            }
            if (action.create || action.retire) {
                const std::string key = recordAddressKey(m_documentRecords.at(id));
                const auto current = m_addressCurrent.find(key);
                if (action.retire && current != m_addressCurrent.end() && current->second == id) {
                    m_addressCurrent.erase(current);
                }
                if (action.create && !action.retire) {
                    m_addressCurrent[key] = id;
                }
            }
        }
    }

    void createDocument(std::int64_t id, const DocumentAction& action, std::int64_t seq) {
        Json record = *action.create;
        record["createdAt"] = seq;
        if (action.retire) {
            record["retiredAt"] = seq;
        }
        Json revision = *action.content;
        revision["seq"] = seq;
        m_recordTypes[id] = "document";
        m_documentRevisions[id] = std::vector<Json>{std::move(revision)};
        m_addressIds[recordAddressKey(record)].insert(id);
        m_documentIdsByScope[scopeKey(record["scope"])].insert(id);
        m_documentRecords[id] = std::move(record);
        m_nextId = std::max(m_nextId, id + 1);
    }

    const std::set<std::int64_t> m_noIds;
    std::mutex m_mutex;
    std::map<std::int64_t, std::string> m_recordTypes;
    std::map<std::int64_t, Json> m_conversations;
    std::set<std::int64_t> m_conversationIds;
    std::map<std::int64_t, std::set<std::int64_t>> m_conversationsByOwnerConversation;
    std::map<std::int64_t, std::set<std::int64_t>> m_conversationsByOwnerTask;
    std::map<std::int64_t, Json> m_entries;
    std::map<std::int64_t, std::set<std::int64_t>> m_entryIds;
    std::map<std::int64_t, std::set<std::int64_t>> m_headEntryIds;
    std::map<std::int64_t, std::int64_t> m_entryCommitSeqs;
    std::map<std::int64_t, Json> m_tasks;
    std::set<std::int64_t> m_taskIds;
    std::map<std::string, std::set<std::int64_t>> m_taskIdsByStatus;
    std::map<std::int64_t, Json> m_submissions;
    std::set<std::int64_t> m_submissionIds;
    std::map<std::string, std::set<std::int64_t>> m_submissionIdsByStatus;
    std::map<std::int64_t, std::map<std::string, std::int64_t>> m_submissionIdsByRequest;
    std::map<std::int64_t, Json> m_documentRecords;
    std::map<std::int64_t, std::vector<Json>> m_documentRevisions;
    std::map<std::string, std::set<std::int64_t>> m_addressIds;
    std::map<std::string, std::int64_t> m_addressCurrent;
    std::map<std::string, std::set<std::int64_t>> m_documentIdsByScope;
    std::int64_t m_nextId = 2;
    std::int64_t m_nextSeq = 1;
    bool m_closed = false;
};
