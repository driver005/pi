export module pi.support.agent_event_watch;

import std;
export import pi.support.agent_event_translator;
export import pi.support.conversation_views;
export import pi.types.result;

/**
 * A serialized stream of one conversation's agent events (spec section 9.4): the `snapshot` at attachment, then one batch of
 * events per publication, delivered in order by a thread of its own. Attachment captures the snapshot and registers for later
 * publications atomically on the session line. At most 100 batches wait for a slow listener: another one replaces all of them
 * by a single `snapshot` of the newest view, so the stream converges but does not promise every transition. The watch ends
 * with a reason: "stopped" (stop()) or "session_closed". Counterpart of watchEvents in events.ts.
 */
export class AgentEventWatch {
public:
    using Listener = std::function<void(const std::vector<Json>& events)>;
    static constexpr std::size_t kMaxPendingBatches = 100;

    AgentEventWatch(ConversationViews& views, std::int64_t conversationId)
        : m_views(views),
          m_conversationId(conversationId) {}

    AgentEventWatch(const AgentEventWatch&) = delete;
    AgentEventWatch& operator=(const AgentEventWatch&) = delete;

    ~AgentEventWatch() {
        (void)stop();
    }

    /** Holds the conversation's view and observes it from its current state. */
    Result<void> attach() {
        auto held = m_views.state(m_conversationId);
        if (!held) {
            return std::unexpected(held.error());
        }
        m_hold = *held;
        auto observation = m_views.observe(
            m_conversationId, [this](IStorage& storage, const Json& value) { return hydrate(storage, value); },
            [this](const Json& before, const Json& after, const Json& ops, const Json& publication) { publish(before, after, ops, publication); }, [this] { end("session_closed"); });
        if (!observation) {
            m_hold.reset();
            return std::unexpected(observation.error());
        }
        m_observer = observation->id;
        return {};
    }

    /** The `snapshot` event at attachment. */
    Json snapshot() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_snapshot;
    }

    /** Starts delivering batches (those already waiting first) to `listener` on the watch's thread. */
    void start(Listener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started || m_end) {
            return;
        }
        m_started = true;
        m_listener = std::move(listener);
        m_thread = std::thread([this] { deliver(); });
    }

    /** Ends the watch and waits for the delivery in progress; returns why the watch ended. Idempotent. */
    std::string stop() {
        end("stopped");
        std::thread finished;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
                finished = std::move(m_thread);
            }
        }
        if (finished.joinable()) {
            finished.join();
        }
        return reason();
    }

    /** Blocks until the watch ended and returns why. */
    std::string wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_end.has_value(); });
        return *m_end;
    }

private:
    /** On the session line: the snapshot, and the generations whose turn already ended at a `completing` hold. */
    Result<void> hydrate(IStorage& storage, const Json& value) {
        std::set<std::int64_t> held;
        std::optional<Json> cursor;
        do {
            TaskQuery query;
            query.conversationId = m_conversationId;
            query.kind = "pi.generation";
            query.status = "completing";
            auto page = storage.scanTasks(query, 100, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const Json& record : page->items) {
                held.insert(record.at("id").get<std::int64_t>());
            }
            cursor = page->next;
        } while (cursor);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_held = std::move(held);
        m_current = value;
        m_snapshot = m_translator.snapshot(value);
        return {};
    }

    void publish(const Json&, const Json& after, const Json&, const Json& publication) {
        Json before;
        std::set<std::int64_t> held;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            before = m_current;
            held = m_held;
        }
        std::vector<Json> events = m_translator.translate(m_conversationId, before, after, publication, held);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_current = after;
        m_held = std::move(held);
        if (events.empty() || m_end) {
            return;
        }
        if (m_pending.size() >= kMaxPendingBatches) {
            m_pending.clear();
            m_pending.push_back({m_translator.snapshot(after)});
        } else {
            m_pending.push_back(std::move(events));
        }
        m_changed.notify_all();
    }

    void deliver() {
        while (true) {
            std::vector<Json> batch;
            Listener listener;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_changed.wait(lock, [this] { return m_end.has_value() || !m_pending.empty(); });
                if (m_end) {
                    return;
                }
                batch = std::move(m_pending.front());
                m_pending.pop_front();
                listener = m_listener;
            }
            listener(batch);
        }
    }

    void end(const std::string& reason) {
        bool first = false;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_end) {
                m_end = reason;
                m_pending.clear();
                first = true;
            }
        }
        m_changed.notify_all();
        if (first && m_observer != 0) {
            m_views.unobserve(m_conversationId, m_observer);
        }
    }

    std::string reason() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_end.value_or("");
    }

    ConversationViews& m_views;
    std::int64_t m_conversationId;
    AgentEventTranslator m_translator;
    std::shared_ptr<IReplicatedState> m_hold;
    std::atomic<std::int64_t> m_observer{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    Json m_snapshot;
    Json m_current;
    std::set<std::int64_t> m_held;
    std::deque<std::vector<Json>> m_pending;
    Listener m_listener;
    std::thread m_thread;
    bool m_started = false;
    std::optional<std::string> m_end;
};
