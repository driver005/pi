module;

#include <cstdint>

export module pi.support.conversation_views;

import std;
export import pi.chord.i_replicated_state;
export import pi.support.builtin_documents;
export import pi.support.context_reader;
export import pi.support.durable_session;
export import pi.support.delta_differ;
export import pi.support.replicated_state;
export import pi.types.doc_address_args;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;
export import pi.types.view_observation;

/**
 * Observable mounts of conversations: the value `{conversation, entries, docs}` of one conversation, where `entries` are
 * the active transcript entries (the newest head marker, then the non-head entries from its head) and `docs` holds the
 * `pi.agent`, `pi.live`, `pi.inbox` and `pi.usage` documents by kind (absent documents are absent). Port of
 * packages/durable/src/harness/view.ts, with the mount a ReplicatedState instead of a Chord observer.
 *
 * A mount is built on the session line by its first observer and lives as long as some caller holds its state. It
 * advances from the session's ordered commit publications, so it never shows uncommitted state; a mount may trail the
 * newest commit until that publication is delivered, and then catches up.
 */
export class ConversationViews {
public:
    /**
     * Called for every publication that reaches the mount, after the mount took it: the view before and after, the view
     * operations of this publication (possibly none) and the publication `{seq, changes}` itself.
     */
    using Observer = std::function<void(const Json& before, const Json& after, const Json& ops, const Json& publication)>;
    /** Reads committed state on the session line when an observer attaches. */
    using Hydrator = std::function<Result<void>(IStorage& storage, const Json& value)>;

    explicit ConversationViews(DurableSession& session)
        : m_session(session) {
        const BuiltinDocuments documents;
        m_definitions = {documents.agent(), documents.live(), documents.inbox(), documents.usage()};
        auto onLine = m_session.subscribeCommitsOnLine([this](const Json& publication) {
            m_lastSeq.store(publication.at("seq").get<std::int64_t>());
        });
        if (onLine) {
            m_lineListener = *onLine;
        }
        auto ordered = m_session.subscribeCommits([this](const Json& publication) { advance(publication); });
        if (ordered) {
            m_commitListener = *ordered;
        }
        auto closed = m_session.subscribeClose([this] {
            std::map<std::int64_t, std::function<void()>> hooks;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_states.clear();
                m_docIds.clear();
                m_built.clear();
                m_observers.clear();
                hooks.swap(m_closeHooks);
            }
            for (const auto& hook : hooks) {
                if (hook.second) {
                    hook.second();
                }
            }
        });
        if (closed) {
            m_closeListener = *closed;
        }
    }

    ConversationViews(const ConversationViews&) = delete;
    ConversationViews& operator=(const ConversationViews&) = delete;

    ~ConversationViews() {
        m_session.unsubscribeCommitsOnLine(m_lineListener);
        m_session.unsubscribeCommits(m_commitListener);
        m_session.unsubscribeClose(m_closeListener);
    }

    /** The mount of a conversation (built when no caller holds one); fails when the conversation does not exist. */
    Result<std::shared_ptr<IReplicatedState>> state(std::int64_t conversationId) {
        std::shared_ptr<ReplicatedState> mounted;
        auto attached = m_session.readOnLine([&]() -> Result<void> {
            mounted = existing(conversationId);
            if (mounted) {
                return {};
            }
            auto built = build(conversationId);
            if (!built) {
                return std::unexpected(built.error());
            }
            mounted = *built;
            return {};
        });
        if (!attached) {
            return std::unexpected(attached.error());
        }
        return std::shared_ptr<IReplicatedState>(mounted);
    }

    /**
     * Registers an observer of the mount of a conversation, atomically on the session line: it sees every publication the
     * mount takes after the returned view and nothing earlier. `hydrate` runs on the line first and may read storage. The
     * caller holds the mount (see state()) for as long as it observes, because a mount nobody holds is dropped.
     */
    Result<ViewObservation> observe(std::int64_t conversationId, const Hydrator& hydrate, Observer observer, std::function<void()> onSessionClosed) {
        ViewObservation observation;
        auto attached = m_session.readOnLine([&]() -> Result<void> {
            const std::shared_ptr<ReplicatedState> mounted = existing(conversationId);
            if (!mounted) {
                return std::unexpected(Error{"durable_error", "Conversation " + std::to_string(conversationId) + " has no held view"});
            }
            observation.value = mounted->snapshot().value;
            if (hydrate) {
                if (auto hydrated = hydrate(m_session.storage(), observation.value); !hydrated) {
                    return hydrated;
                }
            }
            const std::lock_guard<std::mutex> lock(m_mutex);
            observation.id = ++m_nextObserver;
            m_observers[conversationId].emplace(observation.id, std::move(observer));
            m_closeHooks[observation.id] = std::move(onSessionClosed);
            return {};
        });
        if (!attached) {
            return std::unexpected(attached.error());
        }
        return observation;
    }

    void unobserve(std::int64_t conversationId, std::int64_t observerId) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_observers.find(conversationId);
        if (found != m_observers.end()) {
            found->second.erase(observerId);
            if (found->second.empty()) {
                m_observers.erase(found);
            }
        }
        m_closeHooks.erase(observerId);
    }

private:
    std::shared_ptr<ReplicatedState> existing(std::int64_t conversationId) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_states.find(conversationId);
        return found == m_states.end() ? nullptr : found->second.lock();
    }

    /** Reads the committed conversation, active entries and mounted documents; runs on the line. */
    Result<std::shared_ptr<ReplicatedState>> build(std::int64_t conversationId) {
        IStorage& storage = m_session.storage();
        auto conversation = storage.conversation(conversationId);
        if (!conversation) {
            return std::unexpected(conversation.error());
        }
        if (!*conversation) {
            return std::unexpected(Error{"durable_error", "Conversation " + std::to_string(conversationId) + " does not exist"});
        }
        auto bounds = m_reader.capture(storage, conversationId, std::nullopt);
        if (!bounds) {
            return std::unexpected(bounds.error());
        }
        auto entries = m_reader.activeEntries(storage, conversationId, *bounds);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        Json docs = Json::object();
        std::map<std::string, std::int64_t> ids;
        for (const DocDefinition& definition : m_definitions) {
            DocAddressArgs args;
            args.owner = conversationId;
            auto loaded = m_session.loadOnLine(definition, args);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            if (*loaded) {
                docs[definition.kind] = (*loaded)->value;
                ids[definition.kind] = (*loaded)->record.at("id").get<std::int64_t>();
            }
        }
        auto state = std::make_shared<ReplicatedState>(
            Json::object({{"conversation", **conversation}, {"entries", *entries}, {"docs", docs}}));
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_states[conversationId] = state;
        m_docIds[conversationId] = ids;
        m_built[conversationId] = m_lastSeq.load();
        return state;
    }

    /** Applies one ordered publication to every live mount. */
    void advance(const Json& publication) {
        const std::int64_t seq = publication.at("seq").get<std::int64_t>();
        std::vector<std::pair<std::int64_t, std::shared_ptr<ReplicatedState>>> targets;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_states.begin(); it != m_states.end();) {
                std::shared_ptr<ReplicatedState> state = it->second.lock();
                if (!state) {
                    m_docIds.erase(it->first);
                    m_built.erase(it->first);
                    it = m_states.erase(it);
                    continue;
                }
                if (seq > m_built[it->first]) {
                    targets.emplace_back(it->first, std::move(state));
                }
                ++it;
            }
        }
        for (const auto& target : targets) {
            advanceMount(target.first, *target.second, publication);
        }
    }

    void advanceMount(std::int64_t conversationId, ReplicatedState& state, const Json& publication) {
        std::map<std::string, std::int64_t> ids;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ids = m_docIds[conversationId];
        }
        const Json before = state.value();
        (void)state.change(ServiceContext{}, [&](Json& draft) {
            for (const Json& change : publication.at("changes")) {
                const std::string type = change.value("type", std::string());
                if (type == "entry" && change.at("value").value("conversationId", std::int64_t(0)) == conversationId) {
                    applyEntry(draft["entries"], change.at("value"));
                } else if (type == "document" && change.value("conversationId", std::int64_t(0)) == conversationId) {
                    applyDocument(draft["docs"], ids, change);
                }
            }
        });
        std::vector<Observer> observers;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_docIds[conversationId] = ids;
            const auto found = m_observers.find(conversationId);
            if (found != m_observers.end()) {
                for (const auto& entry : found->second) {
                    observers.push_back(entry.second);
                }
            }
        }
        if (observers.empty()) {
            return;
        }
        const Json after = state.value();
        const Json ops = before == after ? Json::array() : m_differ.diff(before, after);
        for (const Observer& observer : observers) {
            observer(before, after, ops, publication);
        }
    }

    /** A head marker keeps the non-head entries from its head, which are always a suffix, and goes in front. */
    void applyEntry(Json& entries, const Json& entry) const {
        if (!entry.contains("head")) {
            entries.push_back(entry);
            return;
        }
        const std::int64_t target = entry.at("head").get<std::int64_t>();
        std::size_t kept = entries.size();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (!entries[index].contains("head") && entries[index].at("id").get<std::int64_t>() >= target) {
                kept = index;
                break;
            }
        }
        Json next = Json::array({entry});
        for (std::size_t index = kept; index < entries.size(); ++index) {
            next.push_back(entries[index]);
        }
        entries = std::move(next);
    }

    void applyDocument(Json& docs, std::map<std::string, std::int64_t>& ids, const Json& change) const {
        const Json& record = change.at("record");
        const std::string kind = record.value("kind", std::string());
        bool mounted = false;
        for (const DocDefinition& definition : m_definitions) {
            mounted = mounted || definition.kind == kind;
        }
        if (!mounted || record.contains("key") || !change.contains("value")) {
            return;
        }
        const std::int64_t id = record.at("id").get<std::int64_t>();
        if (change.at("value").is_null()) {
            auto found = ids.find(kind);
            if (found != ids.end() && found->second == id) {
                ids.erase(found);
                docs.erase(kind);
            }
            return;
        }
        ids[kind] = id;
        docs[kind] = change.at("value");
    }

    DurableSession& m_session;
    ContextReader m_reader;
    DeltaDiffer m_differ;
    std::vector<DocDefinition> m_definitions;
    std::int64_t m_lineListener = 0;
    std::int64_t m_commitListener = 0;
    std::int64_t m_closeListener = 0;
    std::atomic<std::int64_t> m_lastSeq{0};
    std::mutex m_mutex;
    std::map<std::int64_t, std::weak_ptr<ReplicatedState>> m_states;
    std::map<std::int64_t, std::map<std::string, std::int64_t>> m_docIds;
    std::map<std::int64_t, std::int64_t> m_built;
    std::int64_t m_nextObserver = 0;
    std::map<std::int64_t, std::map<std::int64_t, Observer>> m_observers;
    std::map<std::int64_t, std::function<void()>> m_closeHooks;
};
