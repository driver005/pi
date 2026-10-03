module;

#include <cstdint>

export module pi.support.durable_session;

import std;
export import pi.durable.i_storage;
export import pi.durable.i_transaction_host;
export import pi.support.document_resolver;
export import pi.support.transaction;
export import pi.types.doc_address_args;
export import pi.types.doc_definition;
export import pi.types.json;
export import pi.types.loaded_document;
export import pi.types.result;
export import pi.types.transaction_scope;

/**
 * The session kernel over one storage backend: a single mutation line (commits and cache loads run
 * one at a time), the loaded-document cache, and ordered publication of committed changes. Only
 * committed state is observable. Port of packages/durable/src/session/session.ts.
 *
 * A commit callback and its hooks run on the line and must not call back into the session. Line
 * listeners (subscribeCommitsOnLine) run synchronously on the line right after adoption, so state
 * they mirror is exactly the committed state for code running on the line; they must not call back
 * into the session either. Commit listeners run after the line is released, in commit order, on
 * whichever thread finishes a commit (a listener may itself commit). A storage failure other than a clean rejection poisons the
 * session, because memory may no longer match what was stored.
 *
 * Publications are `{seq, changes: [...]}` where a change is a table write (`conversation`, `entry`,
 * `task` or `submission`) or a document change (`document`, `document.copy`).
 */
export class DurableSession : public ITransactionHost {
public:
    using ConversationCreated = std::function<Result<void>(Transaction&, const Json&)>;
    using CommitListener = std::function<void(const Json&)>;

    /**
     * `conversationCreated` runs in every transaction that creates or forks a conversation, after the
     * record is staged; `beforeClose` runs once after close seals admission and before storage closes.
     */
    explicit DurableSession(std::shared_ptr<IStorage> storage, ConversationCreated conversationCreated = nullptr,
                            std::function<void()> beforeClose = nullptr)
        : m_storage(std::move(storage)),
          m_conversationCreated(std::move(conversationCreated)),
          m_beforeClose(std::move(beforeClose)) {}

    /** Runs `change` in a transaction and commits it; returns the commit sequence (0 when nothing was written). */
    Result<std::int64_t> commit(const std::function<Result<void>(Transaction&)>& change, TransactionScope scope = {}) {
        std::int64_t seq = 0;
        {
            std::lock_guard line(m_line);
            if (auto usable = assertUsable(); !usable) {
                return std::unexpected(usable.error());
            }
            auto committed = runCommit(change, scope);
            if (!committed) {
                return std::unexpected(committed.error());
            }
            seq = *committed;
        }
        deliver();
        return seq;
    }

    /** Runs a read-only job on the mutation line so a multi-read derivation sees one committed state. */
    Result<void> readOnLine(const std::function<Result<void>()>& job) {
        std::lock_guard line(m_line);
        if (auto usable = assertUsable(); !usable) {
            return usable;
        }
        return job();
    }

    /** The committed value of a document (a detached copy); null inside the optional when it does not exist. */
    Result<std::optional<Json>> snapshot(const DocDefinition& definition, const DocAddressArgs& args) {
        std::lock_guard line(m_line);
        if (auto usable = assertUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        auto loaded = loadChecked(definition, args);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            return std::optional<Json>();
        }
        return std::optional<Json>((*loaded)->value);
    }

    /**
     * The committed incarnation of a document (record, stored version and value), null when it does not exist. Only
     * for code that already runs on the line: inside readOnLine or a line listener.
     */
    Result<std::shared_ptr<LoadedDocument>> loadOnLine(const DocDefinition& definition, const DocAddressArgs& args) {
        return loadChecked(definition, args);
    }

    /** The value of a conversation document as of one visible entry's commit. */
    Result<std::optional<Json>> snapshotAsOf(const DocDefinition& definition, const DocAddressArgs& args,
                                             std::int64_t entryId) {
        std::lock_guard line(m_line);
        if (auto usable = assertUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        auto resolved = m_resolver.resolve(definition, args);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (resolved->address.scope.at("kind") != "conversation") {
            return std::unexpected(Error{"type_error", "Session.snapshotAsOf() requires a conversation document"});
        }
        const std::int64_t conversationId = resolved->address.scope.at("conversationId").get<std::int64_t>();
        auto entry = m_storage->visibleEntry(conversationId, entryId);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (!*entry) {
            return std::unexpected(Error{"durable_error", "Entry " + std::to_string(entryId) +
                                                              " is not visible from conversation " +
                                                              std::to_string(conversationId)});
        }
        DocumentAddress address = resolved->address;
        address.scope = Json::object(
            {{"kind", "conversation"}, {"conversationId", (*entry)->entry.at("conversationId")}});
        const DocumentPoint point{false, (*entry)->commitSeq};
        return historical(definition, address, point);
    }

    /** Registers a listener called synchronously on the mutation line after each commit is adopted. */
    Result<std::int64_t> subscribeCommitsOnLine(CommitListener listener) {
        std::lock_guard guard(m_listenerMutex);
        if (auto usable = assertUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        const std::int64_t id = ++m_nextListener;
        m_lineListeners[id] = std::move(listener);
        return id;
    }

    void unsubscribeCommitsOnLine(std::int64_t id) {
        std::lock_guard guard(m_listenerMutex);
        m_lineListeners.erase(id);
    }

    /** Registers a listener for committed publications; returns its handle. */
    Result<std::int64_t> subscribeCommits(CommitListener listener) {
        std::lock_guard guard(m_listenerMutex);
        if (auto usable = assertUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        const std::int64_t id = ++m_nextListener;
        m_commitListeners[id] = std::move(listener);
        return id;
    }

    void unsubscribeCommits(std::int64_t id) {
        std::lock_guard guard(m_listenerMutex);
        m_commitListeners.erase(id);
    }

    /** Registers a listener called synchronously when close begins. */
    Result<std::int64_t> subscribeClose(std::function<void()> listener) {
        std::lock_guard guard(m_listenerMutex);
        if (auto usable = assertUsable(); !usable) {
            return std::unexpected(usable.error());
        }
        const std::int64_t id = ++m_nextListener;
        m_closeListeners[id] = std::move(listener);
        return id;
    }

    void unsubscribeClose(std::int64_t id) {
        std::lock_guard guard(m_listenerMutex);
        m_closeListeners.erase(id);
    }

    /** Drops every cached document; later access cold-loads from storage. */
    void unloadDocuments() {
        std::lock_guard line(m_line);
        m_documents.clear();
    }

    /** Seals admission, notifies close listeners, runs `beforeClose`, then closes storage. Idempotent. */
    Result<void> close() {
        std::map<std::int64_t, std::function<void()>> listeners;
        {
            std::lock_guard guard(m_listenerMutex);
            if (m_closing.exchange(true)) {
                return {};
            }
            listeners.swap(m_closeListeners);
        }
        for (const auto& item : listeners) {
            item.second();
        }
        if (m_beforeClose) {
            m_beforeClose();
        }
        std::lock_guard line(m_line);
        {
            std::lock_guard guard(m_listenerMutex);
            m_commitListeners.clear();
            m_lineListeners.clear();
        }
        m_documents.clear();
        return m_storage->close();
    }

    // ─── ITransactionHost (called by transactions while they hold the line) ─

    IStorage& storage() override {
        return *m_storage;
    }

    std::shared_ptr<LoadedDocument> cached(const std::string& addressId) override {
        auto found = m_documents.find(addressId);
        return found == m_documents.end() ? nullptr : found->second;
    }

    Result<std::shared_ptr<LoadedDocument>> load(const DocDefinition& definition, const std::string& addressId,
                                                 const DocumentAddress& address) override {
        if (auto hit = cached(addressId); hit && hit->valueVersion == definition.version) {
            return hit;
        }
        m_documents.erase(addressId);
        auto record = m_storage->findDocument(address, DocumentPoint{true, 0});
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!*record) {
            return std::shared_ptr<LoadedDocument>();
        }
        const std::int64_t id = (*record)->at("id").get<std::int64_t>();
        auto stored = m_storage->document(id, DocumentPoint{true, 0});
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (!*stored) {
            return std::unexpected(Error{"durable_error", "Current document " + std::to_string(id) + " cannot be read"});
        }
        auto value = m_resolver.materialize(definition, (*stored)->record, (*stored)->version, (*stored)->value);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto loaded = std::make_shared<LoadedDocument>();
        loaded->addressId = addressId;
        loaded->record = (*stored)->record;
        loaded->storedVersion = (*stored)->version;
        loaded->valueVersion = definition.version;
        loaded->deltasSinceBase = (*stored)->deltasSinceBase;
        loaded->value = *value;
        m_documents[addressId] = loaded;
        return loaded;
    }

    void install(std::shared_ptr<LoadedDocument> document) override {
        m_documents[document->addressId] = std::move(document);
    }

    void evict(const std::string& addressId, std::int64_t recordId) override {
        auto found = m_documents.find(addressId);
        if (found != m_documents.end() && found->second->record.at("id").get<std::int64_t>() == recordId) {
            m_documents.erase(found);
        }
    }

private:
    Result<std::shared_ptr<LoadedDocument>> loadChecked(const DocDefinition& definition, const DocAddressArgs& args) {
        auto resolved = m_resolver.resolve(definition, args);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        auto loaded = load(definition, resolved->id, resolved->address);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            return std::shared_ptr<LoadedDocument>();
        }
        if (auto scoped = m_resolver.checkScope(definition, (*loaded)->record); !scoped) {
            return std::unexpected(scoped.error());
        }
        if (auto versioned = m_resolver.checkVersion(definition, (*loaded)->record, (*loaded)->storedVersion); !versioned) {
            return std::unexpected(versioned.error());
        }
        return loaded;
    }

    Result<std::int64_t> runCommit(const std::function<Result<void>(Transaction&)>& change, TransactionScope scope) {
        Transaction tx(*this, scope, m_conversationCreated);
        if (auto changed = change(tx); !changed) {
            tx.settleFailure();
            return std::unexpected(changed.error());
        }
        auto writes = tx.settleSuccess();
        if (!writes) {
            return std::unexpected(writes.error());
        }
        if (writes->empty()) {
            return 0;
        }
        auto seq = m_storage->commit(*writes);
        if (!seq) {
            // A clean rejection guarantees that no batch effect committed; anything else may have.
            if (seq.error().code != "storage_rejected") {
                std::lock_guard guard(m_poisonMutex);
                m_poison = seq.error();
            }
            return std::unexpected(seq.error());
        }
        publish(*seq, *writes, tx.adopt(*seq));
        return *seq;
    }

    void publish(std::int64_t seq, const std::vector<Json>& writes, const std::vector<Json>& documents) {
        Json changes = Json::array();
        for (const Json& write : writes) {
            const std::string type = write.at("type").get<std::string>();
            if (type == "conversation" || type == "entry" || type == "task" || type == "submission") {
                changes.push_back(write);
            }
        }
        for (const Json& document : documents) {
            changes.push_back(document);
        }
        Json publication = Json::object({{"seq", seq}, {"changes", changes}});
        std::vector<CommitListener> onLine;
        {
            std::lock_guard guard(m_listenerMutex);
            for (const auto& item : m_lineListeners) {
                onLine.push_back(item.second);
            }
        }
        for (const CommitListener& listener : onLine) {
            listener(publication);
        }
        std::lock_guard guard(m_queueMutex);
        m_queue.push_back(std::move(publication));
    }

    /** Delivers queued publications in order; a nested call (from a listener) leaves them to the outer loop. */
    void deliver() {
        {
            std::lock_guard guard(m_queueMutex);
            if (m_delivering) {
                return;
            }
            m_delivering = true;
        }
        while (true) {
            Json publication;
            {
                std::lock_guard guard(m_queueMutex);
                if (m_queue.empty()) {
                    m_delivering = false;
                    return;
                }
                publication = std::move(m_queue.front());
                m_queue.pop_front();
            }
            std::vector<CommitListener> listeners;
            {
                std::lock_guard guard(m_listenerMutex);
                for (const auto& item : m_commitListeners) {
                    listeners.push_back(item.second);
                }
            }
            for (const CommitListener& listener : listeners) {
                listener(publication);
            }
        }
    }

    Result<std::optional<Json>> historical(const DocDefinition& definition, const DocumentAddress& address,
                                           const DocumentPoint& point) {
        auto record = m_storage->findDocument(address, point);
        if (!record) {
            return std::unexpected(record.error());
        }
        if (!*record) {
            return std::optional<Json>();
        }
        const std::int64_t id = (*record)->at("id").get<std::int64_t>();
        auto stored = m_storage->document(id, point);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (!*stored) {
            return std::unexpected(Error{"durable_error", "Historical document " + std::to_string(id) + " cannot be read"});
        }
        auto value = m_resolver.materialize(definition, (*stored)->record, (*stored)->version, (*stored)->value);
        if (!value) {
            return std::unexpected(value.error());
        }
        return std::optional<Json>(*value);
    }

    Result<void> assertUsable() {
        if (m_closing) {
            return std::unexpected(Error{"session_closed", "Session is closed"});
        }
        std::lock_guard guard(m_poisonMutex);
        if (m_poison) {
            return std::unexpected(Error{"session_poisoned", "Session is poisoned by a failed commit after storage admission; reopen it (" +
                                                                 m_poison->message + ")"});
        }
        return {};
    }

    std::shared_ptr<IStorage> m_storage;
    ConversationCreated m_conversationCreated;
    std::function<void()> m_beforeClose;
    DocumentResolver m_resolver;
    std::mutex m_line;
    std::map<std::string, std::shared_ptr<LoadedDocument>> m_documents;
    std::mutex m_poisonMutex;
    std::optional<Error> m_poison;
    std::atomic<bool> m_closing{false};
    std::mutex m_listenerMutex;
    std::int64_t m_nextListener = 0;
    std::map<std::int64_t, CommitListener> m_commitListeners;
    std::map<std::int64_t, CommitListener> m_lineListeners;
    std::map<std::int64_t, std::function<void()>> m_closeListeners;
    std::mutex m_queueMutex;
    std::deque<Json> m_queue;
    bool m_delivering = false;
};
