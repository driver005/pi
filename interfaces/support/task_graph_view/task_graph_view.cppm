module;

#include <cstdint>

export module pi.support.task_graph_view;

import std;
export import pi.chord.i_replicated_state;
export import pi.support.durable_session;
export import pi.support.replicated_state;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;

/**
 * Observable graph of every live task of the session: `{tasks: {"<id>": node}}` with
 * `node = {id, kind, conversationId, owner?, background, abortRequested, state, conversations}` where `state` is
 * `{status: pending|running, phase}`, `{status: waiting, phase, on, policy}` or `{status: completing, outcome}` (the
 * outcome status only). Terminal tasks leave the graph. Port of packages/durable/src/harness/task-graph.ts, with the
 * mount a ReplicatedState instead of a Chord observer; the lifetime and ordering rules are those of ConversationViews.
 */
export class TaskGraphView {
public:
    static constexpr std::size_t kScanPageSize = 256;

    explicit TaskGraphView(DurableSession& session)
        : m_session(session) {
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
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_state.reset();
        });
        if (closed) {
            m_closeListener = *closed;
        }
    }

    TaskGraphView(const TaskGraphView&) = delete;
    TaskGraphView& operator=(const TaskGraphView&) = delete;

    ~TaskGraphView() {
        m_session.unsubscribeCommitsOnLine(m_lineListener);
        m_session.unsubscribeCommits(m_commitListener);
        m_session.unsubscribeClose(m_closeListener);
    }

    /** The graph mount (built when no caller holds one). */
    Result<std::shared_ptr<IReplicatedState>> state() {
        std::shared_ptr<ReplicatedState> mounted;
        auto attached = m_session.readOnLine([&]() -> Result<void> {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                mounted = m_state.lock();
            }
            if (mounted) {
                return {};
            }
            auto built = build();
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

private:
    Result<std::shared_ptr<ReplicatedState>> build() {
        IStorage& storage = m_session.storage();
        std::vector<Json> records;
        for (const char* status : {"pending", "running", "waiting", "completing"}) {
            std::optional<Json> cursor;
            do {
                TaskQuery query;
                query.status = status;
                auto page = storage.scanTasks(query, kScanPageSize, cursor);
                if (!page) {
                    return std::unexpected(page.error());
                }
                records.insert(records.end(), page->items.begin(), page->items.end());
                cursor = page->next;
            } while (cursor);
        }
        std::sort(records.begin(), records.end(),
                  [](const Json& a, const Json& b) { return a.at("id").get<std::int64_t>() < b.at("id").get<std::int64_t>(); });
        Json tasks = Json::object();
        for (const Json& record : records) {
            auto owned = ownedConversations(storage, record.at("id").get<std::int64_t>());
            if (!owned) {
                return std::unexpected(owned.error());
            }
            tasks[std::to_string(record.at("id").get<std::int64_t>())] = node(record, *owned);
        }
        auto state = std::make_shared<ReplicatedState>(Json::object({{"tasks", tasks}}));
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state;
        m_built = m_lastSeq.load();
        return state;
    }

    Result<Json> ownedConversations(IStorage& storage, std::int64_t taskId) const {
        std::vector<std::int64_t> ids;
        std::optional<Json> cursor;
        do {
            ConversationQuery query;
            query.ownerTaskId = taskId;
            auto page = storage.scanConversations(query, kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const Json& conversation : page->items) {
                ids.push_back(conversation.at("id").get<std::int64_t>());
            }
            cursor = page->next;
        } while (cursor);
        std::sort(ids.begin(), ids.end());
        return Json(ids);
    }

    void advance(const Json& publication) {
        const std::int64_t seq = publication.at("seq").get<std::int64_t>();
        std::shared_ptr<ReplicatedState> state;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            state = m_state.lock();
            if (!state || seq <= m_built) {
                return;
            }
        }
        (void)state->change(ServiceContext{}, [&](Json& draft) { apply(draft.at("tasks"), publication.at("changes")); });
    }

    /** Task changes first, so a conversation created with its owner task in one commit finds the owner's node. */
    void apply(Json& tasks, const Json& changes) const {
        for (const Json& change : changes) {
            if (change.value("type", std::string()) != "task") {
                continue;
            }
            const Json& record = change.at("value");
            const std::string key = std::to_string(record.at("id").get<std::int64_t>());
            const bool present = tasks.contains(key);
            if (record.at("state").at("status") == "terminal") {
                if (present) {
                    tasks.erase(key);
                }
                continue;
            }
            tasks[key] = node(record, present ? tasks.at(key).at("conversations") : Json::array());
        }
        std::map<std::string, std::vector<std::int64_t>> created;
        for (const Json& change : changes) {
            if (change.value("type", std::string()) != "conversation" || !change.at("value").contains("owner")) {
                continue;
            }
            const std::string key = std::to_string(change.at("value").at("owner").at("taskId").get<std::int64_t>());
            if (tasks.contains(key)) {
                created[key].push_back(change.at("value").at("id").get<std::int64_t>());
            }
        }
        for (auto& entry : created) {
            std::vector<std::int64_t> ids = tasks.at(entry.first).at("conversations").get<std::vector<std::int64_t>>();
            ids.insert(ids.end(), entry.second.begin(), entry.second.end());
            std::sort(ids.begin(), ids.end());
            tasks[entry.first]["conversations"] = Json(ids);
        }
    }

    Json node(const Json& record, const Json& conversations) const {
        Json out = Json::object({{"id", record.at("id")}, {"kind", record.at("kind")}, {"conversationId", record.at("conversationId")}});
        if (record.contains("owner")) {
            out["owner"] = record.at("owner");
        }
        out["background"] = record.value("background", false);
        out["abortRequested"] = record.value("abortRequested", false);
        out["state"] = stateOf(record.at("state"));
        out["conversations"] = conversations;
        return out;
    }

    Json stateOf(const Json& state) const {
        const std::string status = state.at("status").get<std::string>();
        if (status == "completing") {
            return Json::object({{"status", "completing"}, {"outcome", state.at("outcome").at("status")}});
        }
        const std::string phase = state.at("checkpoint").value("phase", std::string());
        if (status == "waiting") {
            return Json::object({{"status", "waiting"}, {"phase", phase}, {"on", state.at("on")}, {"policy", state.at("policy")}});
        }
        return Json::object({{"status", status}, {"phase", phase}});
    }

    DurableSession& m_session;
    std::int64_t m_lineListener = 0;
    std::int64_t m_commitListener = 0;
    std::int64_t m_closeListener = 0;
    std::atomic<std::int64_t> m_lastSeq{0};
    std::mutex m_mutex;
    std::weak_ptr<ReplicatedState> m_state;
    std::int64_t m_built = 0;
};
