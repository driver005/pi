export module pi.durable.i_storage;

import std;
export import pi.types.conversation_query;
export import pi.types.document_address;
export import pi.types.document_point;
export import pi.types.document_query;
export import pi.types.entry_lookup;
export import pi.types.entry_query;
export import pi.types.json;
export import pi.types.result;
export import pi.types.storage_page;
export import pi.types.stored_document;
export import pi.types.submission_query;
export import pi.types.task_query;

/**
 * Atomic persistence boundary of the durable session: conversations, entries, tasks, submissions
 * and documents. Records and writes are JSON objects in the shape of packages/durable/src/types.ts
 * (ids and sequences are integers). Storage trusts the owning session to supply semantically valid
 * records; it enforces atomicity, one global id namespace, immutable conversations and entries,
 * document consistency, and hands out detached copies. A commit that fails changes nothing.
 *
 * Writes: `{type: "conversation"|"entry"|"task"|"submission", value}`, `{type: "document.create",
 * record, content}`, `{type: "document.copy", record, source: {id, at}}` (`at` is a sequence or
 * "current"), `{type: "document.change", id, content}`, `{type: "document.retire", id}`; document
 * content is `{kind: "base", version, value}` or `{kind: "delta", version, ops}`. Errors have the
 * code "storage_rejected" (a document copy that cannot be honoured) or "storage_error".
 * Scans take an opaque cursor `{after: id}` and return at most `limit` items.
 */
export class IStorage {
public:
    virtual ~IStorage() = default;

    /** Persists one batch atomically and returns its (strictly increasing) sequence. */
    virtual Result<std::int64_t> commit(const std::vector<Json>& writes) = 0;
    /** A fresh candidate from the session-global id namespace. */
    virtual Result<std::int64_t> mintId() = 0;

    virtual Result<std::optional<Json>> conversation(std::int64_t id) = 0;
    virtual Result<StoragePage> scanConversations(const ConversationQuery& query, std::size_t limit,
                                                  const std::optional<Json>& cursor) = 0;

    /** One entry by global id. */
    virtual Result<std::optional<EntryLookup>> entry(std::int64_t id) = 0;
    /** One entry, only when visible through the conversation's fork ancestry. */
    virtual Result<std::optional<EntryLookup>> visibleEntry(std::int64_t conversationId, std::int64_t id) = 0;
    /** Newest visible entry carrying a `head` at or below the optional inclusive cutoff. */
    virtual Result<std::optional<Json>> findLatestHeadMarker(std::int64_t conversationId,
                                                             const std::optional<std::int64_t>& atOrBeforeEntryId) = 0;
    /** The inclusive visible range, newest first. */
    virtual Result<StoragePage> scanEntries(const EntryQuery& query, std::size_t limit,
                                            const std::optional<Json>& cursor) = 0;

    virtual Result<std::optional<Json>> task(std::int64_t id) = 0;
    virtual Result<StoragePage> scanTasks(const TaskQuery& query, std::size_t limit,
                                          const std::optional<Json>& cursor) = 0;

    virtual Result<std::optional<Json>> submission(std::int64_t id) = 0;
    virtual Result<StoragePage> scanSubmissions(const SubmissionQuery& query, std::size_t limit,
                                                const std::optional<Json>& cursor) = 0;
    virtual Result<std::optional<Json>> submissionByRequest(std::int64_t conversationId,
                                                            const std::string& requestId) = 0;

    virtual Result<std::optional<Json>> findDocument(const DocumentAddress& address, const DocumentPoint& at) = 0;
    /** One incarnation by id, materialized at the point (no replacement at its address is followed). */
    virtual Result<std::optional<StoredDocument>> document(std::int64_t id, const DocumentPoint& at) = 0;
    virtual Result<StoragePage> scanDocuments(const DocumentQuery& query, std::size_t limit,
                                              const std::optional<Json>& cursor) = 0;

    /** Releases resources; every later call fails. */
    virtual Result<void> close() = 0;
};
