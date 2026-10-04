module;

#include <cstdint>

export module pi.support.transaction;

import std;
export import pi.durable.i_transaction_host;
export import pi.support.delta_differ;
export import pi.support.document_resolver;
export import pi.support.fork_planner;
export import pi.types.doc_address_args;
export import pi.types.doc_definition;
export import pi.types.document_entry;
export import pi.types.document_plan;
export import pi.types.json;
export import pi.types.loaded_document;
export import pi.types.result;
export import pi.types.task_options;
export import pi.types.transaction_scope;
export import pi.types.transaction_task;

/**
 * The staged changes of one session commit. A commit callback reads committed state, stages table
 * writes (conversations, entries, tasks, submissions) and edits document drafts; settleSuccess then
 * turns everything into one atomic storage batch, and adopt applies the result to the session's
 * document cache once storage accepted it. Port of packages/durable/src/session/transaction.ts.
 *
 * Reads are refused after the first table write ("read_after_write"): read every row you need first.
 * Document drafts are plain JSON edited in place; the committed operations are derived by diffing the
 * draft against the value as acquired.
 */
export class Transaction {
public:
    static constexpr std::size_t kScanPageSize = 256;
    static constexpr std::int64_t kRootConversationId = 1;

    Transaction(ITransactionHost& host, TransactionScope scope,
                std::function<Result<void>(Transaction&, const Json&)> conversationCreated)
        : m_host(host), m_scope(scope), m_conversationCreated(std::move(conversationCreated)) {}

    // ─── Table reads ────────────────────────────────────────────────────────

    Result<std::optional<Json>> conversation(std::int64_t id) {
        if (auto guard = guardRead("conversation"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().conversation(id);
    }

    Result<std::optional<Json>> entry(std::int64_t id) {
        if (auto guard = guardRead("entry"); !guard) {
            return std::unexpected(guard.error());
        }
        auto found = m_host.storage().entry(id);
        if (!found) {
            return std::unexpected(found.error());
        }
        if (!*found) {
            return std::optional<Json>();
        }
        return std::optional<Json>((*found)->entry);
    }

    Result<std::optional<Json>> task(std::int64_t id) {
        if (auto guard = guardRead("task"); !guard) {
            return std::unexpected(guard.error());
        }
        return committedTask(id);
    }

    Result<StoragePage> scanConversations(const ConversationQuery& query, std::size_t limit,
                                          const std::optional<Json>& cursor) {
        if (auto guard = guardRead("scanConversations"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().scanConversations(query, limit, cursor);
    }

    Result<StoragePage> scanEntries(const EntryQuery& query, std::size_t limit, const std::optional<Json>& cursor) {
        if (auto guard = guardRead("scanEntries"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().scanEntries(query, limit, cursor);
    }

    Result<std::optional<Json>> latestHeadMarker(std::int64_t conversationId) {
        if (auto guard = guardRead("latestHeadMarker"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().findLatestHeadMarker(conversationId, std::nullopt);
    }

    Result<StoragePage> scanTasks(const TaskQuery& query, std::size_t limit, const std::optional<Json>& cursor) {
        if (auto guard = guardRead("scanTasks"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().scanTasks(query, limit, cursor);
    }

    Result<std::optional<Json>> submission(std::int64_t id) {
        if (auto guard = guardRead("submission"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().submission(id);
    }

    Result<std::optional<Json>> submissionByRequest(std::int64_t conversationId, const std::string& requestId) {
        if (auto guard = guardRead("submissionByRequest"); !guard) {
            return std::unexpected(guard.error());
        }
        return m_host.storage().submissionByRequest(conversationId, requestId);
    }

    // ─── Table writes ───────────────────────────────────────────────────────

    /** `ownership` is `{kind: "ownerless"}` or `{kind: "task", taskId}`. */
    Result<Json> createConversation(const Json& ownership) {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        return stageConversation(std::nullopt, ownership, std::nullopt);
    }

    /** The reserved-id bootstrap path of the root conversation. */
    Result<Json> createRootConversation() {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        return stageConversation(std::nullopt, Json::object({{"kind", "ownerless"}}), kRootConversationId);
    }

    Result<Json> forkConversation(std::int64_t parentConversationId, std::int64_t at, const Json& ownership) {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        return stageConversation(std::make_pair(parentConversationId, at), ownership, std::nullopt);
    }

    /** Appends an entry draft (`kind`, `data`, optional `head`; "self" names the entry itself). */
    Result<Json> appendEntry(std::int64_t conversationId, const Json& draft) {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        if (auto required = requireConversation(conversationId); !required) {
            return std::unexpected(required.error());
        }
        auto id = m_host.storage().mintId();
        if (!id) {
            return std::unexpected(id.error());
        }
        Json record = draft;
        record["id"] = *id;
        record["conversationId"] = conversationId;
        if (draft.contains("head") && draft.at("head") == "self") {
            record["head"] = *id;
        }
        if (m_scope.taskId) {
            record["byTaskId"] = *m_scope.taskId;
        }
        m_writes.push_back(Json::object({{"type", "entry"}, {"value", record}}));
        return record;
    }

    /** Creates a task of `name` in the pending state with its initial checkpoint. */
    Result<std::int64_t> createTask(const std::string& name, std::int64_t version, const Json& input,
                                    const Json& checkpoint, const TaskOptions& options) {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        std::optional<Json> owner;
        if (options.ownership.at("kind") == "task") {
            auto found = ownerForTask(options);
            if (!found) {
                return std::unexpected(found.error());
            }
            owner = *found;
        }
        std::optional<std::int64_t> conversationId = options.conversationId ? options.conversationId : m_scope.conversationId;
        if (owner) {
            conversationId = owner->at("conversationId").get<std::int64_t>();
        }
        if (!conversationId) {
            return std::unexpected(Error{"type_error", "Tx.createTask() requires options.conversationId"});
        }
        if (auto required = requireConversation(*conversationId); !required) {
            return std::unexpected(required.error());
        }
        auto id = m_host.storage().mintId();
        if (!id) {
            return std::unexpected(id.error());
        }
        Json record = Json::object({{"id", *id}, {"conversationId", *conversationId}, {"kind", name}, {"version", version}, {"input", input}});
        if (owner) {
            record["owner"] = owner->at("id");
        }
        record["background"] = options.background;
        record["abortRequested"] = false;
        record["state"] = Json::object({{"status", "pending"}, {"checkpoint", checkpoint}});
        TransactionTask& staged = m_tasks[*id];
        staged.writeKind = "create";
        staged.write = record;
        return *id;
    }

    /** Creates a raw submission record with a fresh id; no admission rules apply. */
    Result<Json> createSubmission(const Json& create) {
        if (auto guard = guardWrite(); !guard) {
            return std::unexpected(guard.error());
        }
        if (auto required = requireConversation(create.at("conversationId").get<std::int64_t>()); !required) {
            return std::unexpected(required.error());
        }
        auto id = m_host.storage().mintId();
        if (!id) {
            return std::unexpected(id.error());
        }
        Json record = create;
        record["id"] = *id;
        m_submissions[*id] = record;
        return record;
    }

    /** Settles a submission (`{status: "done", answer}` or `{status: "unanswered", reason, detail?}`). */
    Result<void> settleSubmission(std::int64_t id, const Json& settlement) {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        m_hasTableWrite = true;
        m_submissionChanges.emplace_back(id, settlement);
        return {};
    }

    /** Places a queued submission at `entry`. */
    Result<void> placeSubmission(std::int64_t id, std::int64_t entryId) {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        m_hasTableWrite = true;
        m_submissionChanges.emplace_back(id, Json::object({{"status", "placed"}, {"entry", entryId}}));
        return {};
    }

    /** Replaces one task record completely; tasks change their own state through their runtime. */
    Result<void> setTask(const Json& value) {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        m_hasTableWrite = true;
        TransactionTask& staged = m_tasks[value.at("id").get<std::int64_t>()];
        if (!staged.writeKind.empty()) {
            if (staged.write.at("state").at("status") == "terminal") {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(value.at("id").get<std::int64_t>()) +
                                                                  " already has a terminal candidate"});
            }
            if (staged.write.at("conversationId") != value.at("conversationId")) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(value.at("id").get<std::int64_t>()) +
                                                                  " cannot change conversations"});
            }
        }
        staged.writeKind = staged.writeKind == "create" ? "create" : "replace";
        staged.write = value;
        return {};
    }

    /** Candidate records of the tasks this transaction created or replaced so far. */
    std::vector<Json> stagedTasks() const {
        std::vector<Json> records;
        for (const auto& item : m_tasks) {
            if (!item.second.writeKind.empty()) {
                records.push_back(item.second.write);
            }
        }
        return records;
    }

    /** Conversations this transaction created or forked so far. */
    std::vector<Json> stagedConversations() const {
        std::vector<Json> records;
        for (const Json& write : m_writes) {
            if (write.at("type") == "conversation") {
                records.push_back(write.at("value"));
            }
        }
        return records;
    }

    // ─── Documents ──────────────────────────────────────────────────────────

    /**
     * The editable draft of a document, acquiring (loading or creating) it on first use. The pointer
     * stays valid until the transaction is destroyed; repeated calls for one address return the same draft.
     */
    Result<Json*> doc(const DocDefinition& definition, const DocAddressArgs& args) {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        auto resolved = m_resolver.resolve(definition, args);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (auto open = assertTaskDocumentsOpen(*resolved); !open) {
            return std::unexpected(open.error());
        }
        DocumentEntry* latest = latestAt(resolved->id);
        if (latest != nullptr && !latest->retireOnCommit) {
            if (latest->drafted) {
                return &latest->draft;
            }
            if (latest->target == "fork-copy") {
                if (auto acquired = acquireForkCopy(*latest, definition); !acquired) {
                    return std::unexpected(acquired.error());
                }
                return &latest->draft;
            }
        }
        auto entry = std::make_unique<DocumentEntry>();
        entry->addressId = resolved->id;
        entry->address = resolved->address;
        entry->definition = definition;
        if (auto acquired = acquire(*entry, args.seed, latest != nullptr && latest->retireOnCommit); !acquired) {
            return std::unexpected(acquired.error());
        }
        return &track(std::move(entry))->draft;
    }

    /** Retires the current incarnation of a document; a staged draft is written before it retires. */
    Result<void> retireDoc(const DocDefinition& definition, const DocAddressArgs& args) {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        auto resolved = m_resolver.resolve(definition, args);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        DocumentEntry* latest = latestAt(resolved->id);
        if (latest != nullptr && latest->retireOnCommit) {
            return {};
        }
        if (latest != nullptr && latest->target == "fork-copy") {
            if (auto scoped = m_resolver.checkScope(definition, latest->record); !scoped) {
                return scoped;
            }
            latest->retireOnCommit = true;
            return {};
        }
        if (latest != nullptr && latest->drafted) {
            latest->retireOnCommit = true;
            return {};
        }
        auto entry = std::make_unique<DocumentEntry>();
        entry->addressId = resolved->id;
        entry->address = resolved->address;
        entry->definition = definition;
        entry->retireOnCommit = true;
        if (auto found = findRetirement(*entry); !found) {
            return found;
        }
        track(std::move(entry));
        return {};
    }

    // ─── Settlement ─────────────────────────────────────────────────────────

    /** Seals the transaction after a failed callback; nothing is written. */
    void settleFailure() {
        m_sealed = true;
    }

    /** Seals after a successful callback, derives every document's operations and assembles the batch. */
    Result<std::vector<Json>> settleSuccess() {
        m_sealed = true;
        for (const auto& document : m_documents) {
            if (document->drafted && document->target == "loaded") {
                document->ops = m_differ.diff(document->before, document->draft);
            }
        }
        return assemble();
    }

    /** Applies the committed batch to the document cache; returns the document publications. */
    std::vector<Json> adopt(std::int64_t seq) {
        std::vector<Json> publications;
        for (const DocumentPlan& plan : m_plans) {
            Json record = plan.record;
            if (!plan.committed) {
                record["createdAt"] = seq;
            }
            if (plan.retire) {
                record["retiredAt"] = seq;
            }
            if (plan.change != nullptr) {
                adoptChange(plan, record);
            }
            if (Json publication = publicationOf(plan, record); !publication.is_null()) {
                publications.push_back(std::move(publication));
            }
        }
        return publications;
    }

private:
    // ─── Conversations ──────────────────────────────────────────────────────

    Result<Json> stageConversation(const std::optional<std::pair<std::int64_t, std::int64_t>>& parent,
                                   const Json& ownership, const std::optional<std::int64_t>& reservedId) {
        std::int64_t id = 0;
        if (reservedId) {
            id = *reservedId;
        } else {
            auto minted = m_host.storage().mintId();
            if (!minted) {
                return std::unexpected(minted.error());
            }
            id = *minted;
        }
        Json record = Json::object({{"id", id}});
        if (parent) {
            record["parent"] = Json::object({{"conversationId", parent->first}, {"at", parent->second}});
        }
        if (ownership.at("kind") == "task") {
            auto owner = conversationOwner(ownership.at("taskId").get<std::int64_t>());
            if (!owner) {
                return std::unexpected(owner.error());
            }
            record["owner"] = *owner;
        }
        if (parent) {
            auto copies = m_planner.prepare(m_host.storage(), parent->first, parent->second, id);
            if (!copies) {
                return std::unexpected(copies.error());
            }
            stageForkCopies(*copies);
            m_forkSourceConversationIds.insert(parent->first);
        }
        m_createdConversationIds.insert(id);
        m_writes.push_back(Json::object({{"type", "conversation"}, {"value", record}}));
        if (m_conversationCreated) {
            if (auto created = m_conversationCreated(*this, record); !created) {
                return std::unexpected(created.error());
            }
        }
        return record;
    }

    Result<Json> conversationOwner(std::int64_t taskId) {
        auto task = currentTask(taskId);
        if (!task) {
            return std::unexpected(task.error());
        }
        if (!*task) {
            return std::unexpected(Error{"durable_error", "Conversation owner task " + std::to_string(taskId) + " does not exist"});
        }
        return Json::object({{"conversationId", (*task)->at("conversationId")}, {"taskId", taskId}});
    }

    void stageForkCopies(const std::vector<ForkCopy>& copies) {
        for (const ForkCopy& copy : copies) {
            m_forkSourceDocumentIds.insert(copy.source.at("id").get<std::int64_t>());
            auto entry = std::make_unique<DocumentEntry>();
            entry->addressId = m_resolver.recordAddressId(copy.record);
            entry->address = m_resolver.recordAddress(copy.record);
            entry->target = "fork-copy";
            entry->record = copy.record;
            entry->source = copy.source;
            track(std::move(entry));
        }
    }

    Result<void> requireConversation(std::int64_t id) {
        if (m_createdConversationIds.contains(id)) {
            return {};
        }
        auto found = m_host.storage().conversation(id);
        if (!found) {
            return std::unexpected(found.error());
        }
        if (!*found) {
            return std::unexpected(Error{"durable_error", "Conversation " + std::to_string(id) + " does not exist"});
        }
        return {};
    }

    Result<Json> ownerForTask(const TaskOptions& options) {
        const std::int64_t ownerId = options.ownership.at("taskId").get<std::int64_t>();
        auto owner = currentTask(ownerId);
        if (!owner) {
            return std::unexpected(owner.error());
        }
        if (!*owner) {
            return std::unexpected(Error{"durable_error", "Task owner " + std::to_string(ownerId) + " does not exist"});
        }
        if (options.background) {
            return std::unexpected(Error{"type_error", "A child task cannot be background"});
        }
        const std::int64_t ownerConversation = (*owner)->at("conversationId").get<std::int64_t>();
        if (options.conversationId && *options.conversationId != ownerConversation) {
            return std::unexpected(Error{"durable_error", "A child task lives in its owner's conversation " +
                                                              std::to_string(ownerConversation)});
        }
        return **owner;
    }

    // ─── Tasks ──────────────────────────────────────────────────────────────

    Result<std::optional<Json>> committedTask(std::int64_t id) {
        TransactionTask& staged = m_tasks[id];
        if (!staged.committedRead) {
            auto found = m_host.storage().task(id);
            if (!found) {
                return std::unexpected(found.error());
            }
            staged.committed = *found;
            staged.committedRead = true;
        }
        return staged.committed;
    }

    /** The latest candidate task record, falling back to committed state; not a caller table read. */
    Result<std::optional<Json>> currentTask(std::int64_t id) {
        auto found = m_tasks.find(id);
        if (found != m_tasks.end() && !found->second.writeKind.empty()) {
            return std::optional<Json>(found->second.write);
        }
        return committedTask(id);
    }

    // ─── Documents ──────────────────────────────────────────────────────────

    DocumentEntry* latestAt(const std::string& addressId) {
        auto found = m_latest.find(addressId);
        return found == m_latest.end() ? nullptr : found->second;
    }

    DocumentEntry* track(std::unique_ptr<DocumentEntry> entry) {
        DocumentEntry* raw = entry.get();
        m_documents.push_back(std::move(entry));
        m_latest[raw->addressId] = raw;
        return raw;
    }

    Result<void> assertTaskDocumentsOpen(const ResolvedAddress& resolved) {
        if (resolved.address.scope.at("kind") != "task") {
            return {};
        }
        auto found = m_tasks.find(resolved.address.scope.at("taskId").get<std::int64_t>());
        if (found != m_tasks.end() && !found->second.writeKind.empty() &&
            found->second.write.at("state").at("status") == "terminal") {
            return std::unexpected(Error{"durable_error", "Task " + std::to_string(found->first) + " is terminal"});
        }
        return {};
    }

    Result<void> acquire(DocumentEntry& entry, const Json& seed, bool skipLoad) {
        const DocDefinition& definition = *entry.definition;
        if (!skipLoad) {
            auto loaded = m_host.load(definition, entry.addressId, entry.address);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            if (*loaded) {
                return acquireLoaded(entry, *loaded);
            }
        }
        return acquireNew(entry, seed);
    }

    Result<void> acquireLoaded(DocumentEntry& entry, const std::shared_ptr<LoadedDocument>& loaded) {
        const DocDefinition& definition = *entry.definition;
        if (auto scoped = m_resolver.checkScope(definition, loaded->record); !scoped) {
            return scoped;
        }
        if (auto versioned = m_resolver.checkVersion(definition, loaded->record, loaded->storedVersion); !versioned) {
            return versioned;
        }
        entry.target = "loaded";
        entry.loaded = loaded;
        entry.before = loaded->value;
        entry.draft = loaded->value;
        entry.drafted = true;
        return {};
    }

    Result<void> acquireNew(DocumentEntry& entry, const Json& seed) {
        const DocDefinition& definition = *entry.definition;
        const Json& scope = entry.address.scope;
        if (scope.at("kind") == "conversation") {
            if (auto required = requireConversation(scope.at("conversationId").get<std::int64_t>()); !required) {
                return required;
            }
        } else if (scope.at("kind") == "task") {
            const std::int64_t taskId = scope.at("taskId").get<std::int64_t>();
            auto task = currentTask(taskId);
            if (!task) {
                return std::unexpected(task.error());
            }
            if (!*task) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(taskId) + " does not exist"});
            }
            if ((*task)->at("state").at("status") == "terminal") {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(taskId) + " is terminal"});
            }
        }
        auto id = m_host.storage().mintId();
        if (!id) {
            return std::unexpected(id.error());
        }
        entry.draft = definition.initial ? definition.initial(definition.family ? seed : Json(nullptr)) : Json::object();
        entry.before = entry.draft;
        entry.target = "created";
        entry.record = m_resolver.createRecord(definition, entry.address, *id);
        entry.version = definition.version;
        entry.drafted = true;
        return {};
    }

    Result<void> acquireForkCopy(DocumentEntry& entry, const DocDefinition& definition) {
        const Json& source = entry.source;
        const DocumentPoint at = source.at("at").is_string() ? DocumentPoint{true, 0}
                                                              : DocumentPoint{false, source.at("at").get<std::int64_t>()};
        auto stored = m_host.storage().document(source.at("id").get<std::int64_t>(), at);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        const std::string sourceId = std::to_string(source.at("id").get<std::int64_t>());
        if (!*stored) {
            return std::unexpected(Error{"durable_error", "Fork source document " + sourceId + " cannot be read"});
        }
        if (!sameSemantics((*stored)->record, entry.record)) {
            return std::unexpected(
                Error{"durable_error", "Fork source document " + sourceId + " does not match the copied record"});
        }
        auto value = m_resolver.materialize(definition, entry.record, (*stored)->version, (*stored)->value);
        if (!value) {
            return std::unexpected(value.error());
        }
        entry.definition = definition;
        entry.target = "created";
        entry.version = definition.version;
        entry.draft = *value;
        entry.before = *value;
        entry.drafted = true;
        return {};
    }

    bool sameSemantics(const Json& stored, const Json& copy) const {
        return stored.at("scope").at("kind") == "conversation" && stored.at("kind") == copy.at("kind") &&
               stored.value("key", Json(nullptr)) == copy.value("key", Json(nullptr)) &&
               stored.value("history", Json(nullptr)) == copy.value("history", Json(nullptr)) &&
               stored.value("fork", Json(nullptr)) == copy.value("fork", Json(nullptr));
    }

    Result<void> findRetirement(DocumentEntry& entry) {
        std::optional<Json> record;
        if (auto cached = m_host.cached(entry.addressId)) {
            record = cached->record;
        } else {
            auto found = m_host.storage().findDocument(entry.address, DocumentPoint{true, 0});
            if (!found) {
                return std::unexpected(found.error());
            }
            record = *found;
        }
        if (!record) {
            return {};
        }
        if (auto scoped = m_resolver.checkScope(*entry.definition, *record); !scoped) {
            return scoped;
        }
        entry.target = "retire-only";
        entry.record = *record;
        return {};
    }

    // ─── Assembly ───────────────────────────────────────────────────────────

    Result<std::vector<Json>> assemble() {
        for (const auto& document : m_documents) {
            if (auto plan = planDocument(*document)) {
                m_plans.push_back(std::move(*plan));
            }
        }
        if (auto rejected = rejectForkSourceWrites(); !rejected) {
            return std::unexpected(rejected.error());
        }
        if (auto owners = validateOwners(); !owners) {
            return std::unexpected(owners.error());
        }
        if (auto replaced = checkReplacedTasks(); !replaced) {
            return std::unexpected(replaced.error());
        }
        if (auto retired = retireTerminalTaskDocuments(); !retired) {
            return std::unexpected(retired.error());
        }
        if (auto owners = resolvePublicationOwners(); !owners) {
            return std::unexpected(owners.error());
        }
        if (auto changes = applySubmissionChanges(); !changes) {
            return std::unexpected(changes.error());
        }
        return collectWrites();
    }

    std::optional<DocumentPlan> planDocument(DocumentEntry& entry) {
        if (entry.target == "none") {
            return std::nullopt;
        }
        DocumentPlan plan;
        plan.addressId = entry.addressId;
        plan.retire = entry.retireOnCommit;
        if (entry.target == "created") {
            plan.record = entry.record;
            plan.content = Json::object(
                {{"type", "document.create"},
                 {"record", entry.record},
                 {"content", Json::object({{"version", entry.version}, {"kind", "base"}, {"value", entry.draft}})}});
            plan.change = &entry;
        } else if (entry.target == "fork-copy") {
            plan.record = entry.record;
            plan.content = Json::object({{"type", "document.copy"}, {"record", entry.record}, {"source", entry.source}});
        } else if (entry.target == "retire-only") {
            plan.record = entry.record;
            plan.committed = true;
        } else {
            plan.record = entry.loaded->record;
            plan.committed = true;
            plan.content = loadedContent(entry);
            plan.change = &entry;
        }
        return plan;
    }

    /** A version change stores a base even without operations; otherwise only a change stores a delta. */
    Json loadedContent(const DocumentEntry& entry) const {
        const std::int64_t version = entry.definition->version;
        const std::int64_t id = entry.loaded->record.at("id").get<std::int64_t>();
        if (entry.loaded->storedVersion < version) {
            return Json::object({{"type", "document.change"},
                                 {"id", id},
                                 {"content", Json::object({{"version", version}, {"kind", "base"}, {"value", entry.draft}})}});
        }
        if (!entry.ops.empty()) {
            return Json::object({{"type", "document.change"},
                                 {"id", id},
                                 {"content", Json::object({{"version", version}, {"kind", "delta"}, {"ops", entry.ops}})}});
        }
        return nullptr;
    }

    /** Whether adoption publishes the plan: creations, copies, retirements and loaded documents that wrote. */
    bool publishes(const DocumentPlan& plan) const {
        return plan.retire || plan.change == nullptr || plan.change->target != "loaded" || !plan.content.is_null();
    }

    Result<void> rejectForkSourceWrites() const {
        for (const DocumentPlan& plan : m_plans) {
            if (plan.content.is_null() && !plan.retire) {
                continue;
            }
            const std::int64_t id = plan.record.at("id").get<std::int64_t>();
            if (m_forkSourceDocumentIds.contains(id)) {
                return std::unexpected(Error{"durable_error", "Cannot change fork source document " + std::to_string(id) +
                                                                  " in the fork transaction"});
            }
            const Json& scope = plan.record.at("scope");
            if (scope.at("kind") == "conversation" &&
                m_forkSourceConversationIds.contains(scope.at("conversationId").get<std::int64_t>()) &&
                plan.record.value("fork", std::string()) == "current") {
                return std::unexpected(Error{"durable_error", "Cannot fork conversation " +
                                                                  std::to_string(scope.at("conversationId").get<std::int64_t>()) +
                                                                  " while changing its current-policy documents"});
            }
        }
        return {};
    }

    /** New owned work needs a live owner, judged on the owner's final candidate. */
    Result<void> validateOwners() {
        std::vector<std::pair<std::string, std::int64_t>> owners;
        for (const Json& write : m_writes) {
            if (write.at("type") == "conversation" && write.at("value").contains("owner")) {
                owners.emplace_back("Conversation owner task", write.at("value").at("owner").at("taskId").get<std::int64_t>());
            }
        }
        for (const auto& item : m_tasks) {
            if (item.second.writeKind == "create" && item.second.write.contains("owner")) {
                owners.emplace_back("Task owner", item.second.write.at("owner").get<std::int64_t>());
            }
        }
        for (const auto& [what, taskId] : owners) {
            auto task = currentTask(taskId);
            if (!task) {
                return std::unexpected(task.error());
            }
            const std::string label = what + " " + std::to_string(taskId);
            if (!*task) {
                return std::unexpected(Error{"durable_error", label + " does not exist"});
            }
            const std::string status = (*task)->at("state").at("status").get<std::string>();
            if (status == "terminal" || status == "completing") {
                return std::unexpected(Error{"durable_error", label + " is " + status});
            }
            if ((*task)->value("abortRequested", false)) {
                return std::unexpected(Error{"durable_error", label + " is abort-marked"});
            }
        }
        return {};
    }

    Result<void> checkReplacedTasks() {
        std::vector<std::int64_t> replaced;
        for (const auto& item : m_tasks) {
            if (item.second.writeKind == "replace") {
                replaced.push_back(item.first);
            }
        }
        for (const std::int64_t id : replaced) {
            auto committed = committedTask(id);
            if (!committed) {
                return std::unexpected(committed.error());
            }
            const std::string label = "Task " + std::to_string(id);
            if (!*committed) {
                return std::unexpected(Error{"durable_error", label + " does not exist"});
            }
            if ((*committed)->at("state").at("status") == "terminal") {
                return std::unexpected(Error{"durable_error", label + " is already terminal"});
            }
            if ((*committed)->at("conversationId") != m_tasks[id].write.at("conversationId")) {
                return std::unexpected(Error{"durable_error", label + " cannot change conversations"});
            }
        }
        return {};
    }

    /** Terminal settlement retires every task document, including ones created by this transaction. */
    Result<void> retireTerminalTaskDocuments() {
        std::set<std::int64_t> terminal;
        for (const auto& item : m_tasks) {
            if (!item.second.writeKind.empty() && item.second.write.at("state").at("status") == "terminal") {
                terminal.insert(item.first);
            }
        }
        if (terminal.empty()) {
            return {};
        }
        std::set<std::int64_t> retiring;
        for (DocumentPlan& plan : m_plans) {
            const Json& scope = plan.record.at("scope");
            if (scope.at("kind") == "task" && terminal.contains(scope.at("taskId").get<std::int64_t>())) {
                plan.retire = true;
                retiring.insert(plan.record.at("id").get<std::int64_t>());
            }
        }
        for (const std::int64_t taskId : terminal) {
            if (m_tasks[taskId].writeKind == "create") {
                continue;
            }
            if (auto scanned = retireScannedTaskDocuments(taskId, retiring); !scanned) {
                return scanned;
            }
        }
        return {};
    }

    Result<void> retireScannedTaskDocuments(std::int64_t taskId, std::set<std::int64_t>& retiring) {
        std::optional<Json> cursor;
        do {
            const Json scope = Json::object({{"kind", "task"}, {"taskId", taskId}});
            auto page = m_host.storage().scanDocuments(DocumentQuery{scope, DocumentPoint{true, 0}, std::nullopt},
                                                       kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const Json& record : page->items) {
                if (!retiring.insert(record.at("id").get<std::int64_t>()).second) {
                    continue;
                }
                DocumentPlan plan;
                plan.addressId = m_resolver.recordAddressId(record);
                plan.record = record;
                plan.committed = true;
                plan.retire = true;
                m_plans.push_back(std::move(plan));
            }
            cursor = page->next;
        } while (cursor);
        return {};
    }

    /** Resolves publication ownership before storage admission so adoption stays synchronous. */
    Result<void> resolvePublicationOwners() {
        for (DocumentPlan& plan : m_plans) {
            if (!publishes(plan)) {
                continue;
            }
            const Json& scope = plan.record.at("scope");
            if (scope.at("kind") == "conversation") {
                plan.conversationId = scope.at("conversationId").get<std::int64_t>();
            } else if (scope.at("kind") == "task") {
                auto owner = publicationConversation(scope.at("taskId").get<std::int64_t>());
                if (!owner) {
                    return std::unexpected(owner.error());
                }
                plan.conversationId = *owner;
            }
        }
        return {};
    }

    Result<std::optional<std::int64_t>> publicationConversation(std::int64_t taskId) {
        TransactionTask& staged = m_tasks[taskId];
        if (!staged.publicationConversationId) {
            auto current = currentTask(taskId);
            if (!current) {
                return std::unexpected(current.error());
            }
            if (*current) {
                staged.publicationConversationId = (*current)->at("conversationId").get<std::int64_t>();
            }
        }
        return staged.publicationConversationId;
    }

    Result<void> applySubmissionChanges() {
        for (const auto& [id, change] : m_submissionChanges) {
            std::optional<Json> current;
            if (auto created = m_submissions.find(id); created != m_submissions.end()) {
                current = created->second;
            } else {
                auto stored = m_host.storage().submission(id);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                current = *stored;
            }
            if (!current) {
                return std::unexpected(Error{"durable_error", "Submission " + std::to_string(id) + " does not exist"});
            }
            auto next = nextSubmission(*current, change);
            if (!next) {
                return std::unexpected(next.error());
            }
            if (*next) {
                m_submissions[id] = **next;
            }
        }
        return {};
    }

    /**
     * The record after one change, or nothing when the record is already settled. Placement turns a
     * queued input `placed` and a queued write `done`; only a placed input can be answered.
     */
    Result<std::optional<Json>> nextSubmission(const Json& current, const Json& change) const {
        const std::string status = current.at("status").get<std::string>();
        const std::string label = "Submission " + std::to_string(current.at("id").get<std::int64_t>());
        if (status == "done" || status == "unanswered") {
            return std::optional<Json>();
        }
        Json next = current;
        if (change.at("status") == "placed") {
            if (status != "queued") {
                return std::unexpected(Error{"durable_error", label + " is not queued"});
            }
            next["status"] = current.at("type") == "input" ? "placed" : "done";
            next["entry"] = change.at("entry");
            return std::optional<Json>(next);
        }
        if (change.at("status") == "done" && status != "placed") {
            return std::unexpected(Error{"durable_error", label + " is not a placed input"});
        }
        for (const auto& item : change.items()) {
            next[item.key()] = item.value();
        }
        return std::optional<Json>(next);
    }

    std::vector<Json> collectWrites() {
        std::vector<Json> writes = m_writes;
        for (const auto& item : m_submissions) {
            writes.push_back(Json::object({{"type", "submission"}, {"value", item.second}}));
        }
        for (const auto& item : m_tasks) {
            if (!item.second.writeKind.empty()) {
                writes.push_back(Json::object({{"type", "task"}, {"value", item.second.write}}));
            }
        }
        for (DocumentPlan& plan : m_plans) {
            applyCheckpoint(plan);
            if (!plan.content.is_null()) {
                writes.push_back(plan.content);
            }
            if (plan.retire) {
                writes.push_back(Json::object({{"type", "document.retire"}, {"id", plan.record.at("id")}}));
            }
        }
        return writes;
    }

    /** Checkpoint predicates run last, after every validation. */
    void applyCheckpoint(DocumentPlan& plan) {
        if (plan.content.is_null() || plan.change == nullptr || plan.change->target != "loaded" ||
            plan.content.at("type") != "document.change" || plan.content.at("content").at("kind") != "delta") {
            return;
        }
        const DocumentEntry& entry = *plan.change;
        if (entry.definition->checkpointWhen &&
            entry.definition->checkpointWhen(entry.draft, entry.ops, entry.loaded->deltasSinceBase)) {
            plan.content["content"] = Json::object(
                {{"version", entry.definition->version}, {"kind", "base"}, {"value", entry.draft}});
        }
    }

    // ─── Adoption ───────────────────────────────────────────────────────────

    void adoptChange(const DocumentPlan& plan, const Json& record) {
        DocumentEntry& entry = *plan.change;
        if (entry.target == "loaded") {
            const std::int64_t version = entry.definition->version;
            LoadedDocument& loaded = *entry.loaded;
            if (!entry.ops.empty()) {
                loaded.value = entry.draft;
            }
            if (loaded.storedVersion < version) {
                loaded.storedVersion = version;
            }
            if (!plan.content.is_null() && plan.content.at("type") == "document.change") {
                loaded.deltasSinceBase = plan.content.at("content").at("kind") == "base" ? 0 : loaded.deltasSinceBase + 1;
            }
        } else if (!plan.retire) {
            auto installed = std::make_shared<LoadedDocument>();
            installed->addressId = plan.addressId;
            installed->record = record;
            installed->storedVersion = entry.version;
            installed->valueVersion = entry.version;
            installed->deltasSinceBase = 0;
            installed->value = entry.draft;
            m_host.install(std::move(installed));
        }
    }

    Json publicationOf(const DocumentPlan& plan, const Json& record) {
        Json publication = Json::object({{"type", "document"}, {"record", record}});
        if (plan.conversationId) {
            publication["conversationId"] = *plan.conversationId;
        }
        if (plan.retire) {
            if (plan.committed) {
                m_host.evict(plan.addressId, record.at("id").get<std::int64_t>());
            }
            publication["value"] = nullptr;
            publication["ops"] = Json::array();
            return publication;
        }
        if (!plan.content.is_null() && plan.content.at("type") == "document.copy") {
            publication["type"] = "document.copy";
            publication["source"] = plan.content.at("source");
            return publication;
        }
        if (plan.change != nullptr && publishes(plan)) {
            const DocumentEntry& entry = *plan.change;
            const bool loaded = entry.target == "loaded";
            publication["version"] = loaded ? entry.definition->version : entry.version;
            publication["value"] = entry.draft;
            publication["ops"] = loaded ? entry.ops : Json::array();
            return publication;
        }
        return nullptr;
    }

    // ─── Guards ─────────────────────────────────────────────────────────────

    Result<void> assertOpen() const {
        if (m_sealed) {
            return std::unexpected(Error{"transaction_settled", "Transaction has settled"});
        }
        return {};
    }

    Result<void> guardRead(const std::string& method) const {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        if (m_hasTableWrite) {
            return std::unexpected(
                Error{"read_after_write", "Tx." + method + "() cannot read tables after the first table write"});
        }
        return {};
    }

    Result<void> guardWrite() {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        m_hasTableWrite = true;
        return {};
    }

    ITransactionHost& m_host;
    TransactionScope m_scope;
    std::function<Result<void>(Transaction&, const Json&)> m_conversationCreated;
    DeltaDiffer m_differ;
    DocumentResolver m_resolver;
    ForkPlanner m_planner;
    bool m_sealed = false;
    bool m_hasTableWrite = false;
    /** Atomic batch; conversation and entry writes stage eagerly, task and document writes assemble later. */
    std::vector<Json> m_writes;
    std::set<std::int64_t> m_createdConversationIds;
    std::set<std::int64_t> m_forkSourceConversationIds;
    std::set<std::int64_t> m_forkSourceDocumentIds;
    std::map<std::int64_t, TransactionTask> m_tasks;
    std::map<std::int64_t, Json> m_submissions;
    std::vector<std::pair<std::int64_t, Json>> m_submissionChanges;
    std::vector<DocumentPlan> m_plans;
    std::vector<std::unique_ptr<DocumentEntry>> m_documents;
    std::map<std::string, DocumentEntry*> m_latest;
};
