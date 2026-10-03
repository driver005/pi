module;

#include <cstdint>

export module pi.testing.task_fixture;

import std;
export import pi.durable.memory_storage;
export import pi.support.builtin_documents;
export import pi.support.durable_session;
export import pi.support.registry;
export import pi.support.resolved_agent;
export import pi.support.task_scheduler;

/**
 * A durable session over MemoryStorage with a registry and a running task scheduler, for task and harness tests: it
 * wraps the plumbing (conversations, tasks, entries, polling) so tests read as scenarios.
 */
export class TaskFixture {
public:
    explicit TaskFixture(std::vector<std::shared_ptr<const TaskDefinition>> builtins = {})
        : m_storage(std::make_shared<MemoryStorage>()), m_registry(std::move(builtins)) {}

    ~TaskFixture() {
        close();
    }

    /** The agent a conversation's tasks resolve; default: an agent with no tools. */
    void setAgentFactory(std::function<std::shared_ptr<const IConversationAgent>(std::int64_t)> factory) {
        m_agentFactory = std::move(factory);
    }

    void setModels(IModelRuntime* models) {
        m_models = models;
    }

    void setSettings(const ResolvedSettings& settings) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
    }

    void setOutcomeCleanup(std::function<Result<void>(Transaction&, const Json&, const Json&)> cleanup) {
        m_cleanup = std::move(cleanup);
    }

    void setContextReader(std::function<Result<Json>(std::int64_t, const std::optional<std::int64_t>&)> reader) {
        m_context = std::move(reader);
    }

    /** Opens the session and the scheduler (not yet resumed). */
    void open() {
        m_session = std::make_unique<DurableSession>(m_storage, nullptr, [this] {
            if (m_scheduler) {
                m_scheduler->join();
            }
        });
        SchedulerCallbacks callbacks;
        callbacks.agent = [this](std::int64_t conversationId, const std::shared_ptr<const IRegistrySnapshot>&) -> Result<std::shared_ptr<const IConversationAgent>> {
            return m_agentFactory ? m_agentFactory(conversationId)
                                  : std::shared_ptr<const IConversationAgent>(std::make_shared<ResolvedAgent>(std::make_shared<AgentSnapshot>(),
                                                                                                    std::vector<PromptSection>{},
                                                                                                    std::vector<std::shared_ptr<const Extension>>{}));
        };
        callbacks.settings = [this] {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_settings;
        };
        callbacks.models = [this] { return m_models; };
        callbacks.env = [](std::int64_t) -> Result<std::shared_ptr<IExecutionEnv>> { return std::shared_ptr<IExecutionEnv>(); };
        callbacks.now = [this] { return m_clock.load(); };
        callbacks.report = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_reports.push_back(error.message);
        };
        callbacks.settleOutcome = [this](Transaction& tx, const Json& record, const Json& outcome) -> Result<void> {
            return m_cleanup ? m_cleanup(tx, record, outcome) : Result<void>();
        };
        callbacks.withdrawInputs = [](Transaction&, std::int64_t) -> Result<void> { return {}; };
        callbacks.conversation = [](std::int64_t, const std::shared_ptr<TaskInvocation>&) -> Result<std::shared_ptr<IConversationHandle>> {
            return std::shared_ptr<IConversationHandle>();
        };
        callbacks.context = [this](std::int64_t conversationId, const std::optional<std::int64_t>& at) -> Result<Json> {
            if (m_context) {
                return m_context(conversationId, at);
            }
            return Json::object();
        };
        m_scheduler = std::make_unique<TaskScheduler>(*m_session, m_registry, std::move(callbacks));
        (void)m_scheduler->open();
    }

    void close() {
        if (m_session) {
            (void)m_session->close();
        }
        m_scheduler.reset();
        m_session.reset();
    }

    void install(const std::string& name, std::vector<std::shared_ptr<const TaskDefinition>> tasks) {
        Extension extension;
        extension.name = name;
        extension.tasks = std::move(tasks);
        (void)m_registry.install(std::move(extension));
    }

    void resume() {
        m_scheduler->resume();
    }

    DurableSession& session() {
        return *m_session;
    }

    TaskScheduler& scheduler() {
        return *m_scheduler;
    }

    Registry& registry() {
        return m_registry;
    }

    std::shared_ptr<MemoryStorage> storage() {
        return m_storage;
    }

    std::atomic<std::int64_t>& clock() {
        return m_clock;
    }

    std::int64_t newConversation(const Json& ownership = Json::object({{"kind", "ownerless"}})) {
        std::int64_t id = 0;
        (void)m_session->commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(ownership);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        });
        return id;
    }

    /** Appends an entry draft and returns its id. */
    std::int64_t append(std::int64_t conversationId, const Json& draft) {
        std::int64_t id = 0;
        (void)m_session->commit([&](Transaction& tx) -> Result<void> {
            auto entry = tx.appendEntry(conversationId, draft);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            id = entry->at("id").get<std::int64_t>();
            return {};
        });
        return id;
    }

    /** Creates a conversation-owned task of a registered definition; returns its id. */
    std::int64_t createTask(std::int64_t conversationId, const std::string& name, const Json& input,
                            const Json& ownership = Json::object({{"kind", "conversation"}}), bool background = false) {
        auto definition = m_registry.snapshot()->task(name);
        TaskOptions options;
        options.ownership = ownership;
        options.conversationId = conversationId;
        options.background = background;
        std::int64_t id = 0;
        (void)m_session->commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createTask(name, definition->version, input, definition->initial(input), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return {};
        });
        return id;
    }

    std::optional<Json> task(std::int64_t id) {
        std::optional<Json> record;
        (void)m_session->readOnLine([&]() -> Result<void> {
            auto stored = m_storage->task(id);
            if (stored) {
                record = *stored;
            }
            return {};
        });
        return record;
    }

    std::string status(std::int64_t id) {
        auto record = task(id);
        return record ? record->at("state").at("status").get<std::string>() : "missing";
    }

    /** The outcome status of a terminal task, or "" while it is live. */
    std::string outcome(std::int64_t id) {
        auto record = task(id);
        if (!record || record->at("state").at("status") != "terminal") {
            return "";
        }
        return record->at("state").at("outcome").at("status").get<std::string>();
    }

    bool terminal(std::int64_t id) {
        return status(id) == "terminal";
    }

    std::optional<Json> submission(std::int64_t id) {
        std::optional<Json> record;
        (void)m_session->readOnLine([&]() -> Result<void> {
            auto stored = m_storage->submission(id);
            if (stored) {
                record = *stored;
            }
            return {};
        });
        return record;
    }

    /** The committed value of a conversation document, or an empty object when absent. */
    Json document(const DocDefinition& definition, std::int64_t conversationId) {
        DocAddressArgs args;
        args.owner = conversationId;
        auto snapshot = m_session->snapshot(definition, args);
        return snapshot && *snapshot ? **snapshot : Json::object();
    }

    /** Every entry of a conversation, oldest first. */
    std::vector<Json> entries(std::int64_t conversationId) {
        std::vector<Json> all;
        (void)m_session->readOnLine([&]() -> Result<void> {
            std::optional<Json> cursor;
            do {
                auto page = m_storage->scanEntries(EntryQuery{conversationId, std::nullopt, std::nullopt}, 256, cursor);
                if (!page) {
                    return {};
                }
                all.insert(all.end(), page->items.begin(), page->items.end());
                cursor = page->next;
            } while (cursor);
            return {};
        });
        std::reverse(all.begin(), all.end());
        return all;
    }

    std::vector<std::string> reports() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_reports;
    }

    /** Polls until `predicate` holds (up to ten seconds). */
    template <typename Predicate>
    bool eventually(Predicate predicate) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

private:
    std::shared_ptr<MemoryStorage> m_storage;
    Registry m_registry;
    std::unique_ptr<DurableSession> m_session;
    std::unique_ptr<TaskScheduler> m_scheduler;
    std::function<std::shared_ptr<const IConversationAgent>(std::int64_t)> m_agentFactory;
    std::function<Result<void>(Transaction&, const Json&, const Json&)> m_cleanup;
    std::function<Result<Json>(std::int64_t, const std::optional<std::int64_t>&)> m_context;
    IModelRuntime* m_models = nullptr;
    ResolvedSettings m_settings;
    std::atomic<std::int64_t> m_clock{1000};
    std::mutex m_mutex;
    std::vector<std::string> m_reports;
};
