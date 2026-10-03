module;

#include <cstdint>

export module pi.support.harness;

import std;
export import pi.durable.i_conversation_host;
export import pi.durable.i_storage;
export import pi.support.agent_configurator;
export import pi.support.agent_resolver;
export import pi.support.bound_conversation;
export import pi.support.builtin_documents;
export import pi.support.builtin_tasks;
export import pi.support.compaction_planner;
export import pi.support.context_reader;
export import pi.support.conversation;
export import pi.support.durable_session;
export import pi.support.inbox_boundary;
export import pi.support.outcome_settlement;
export import pi.support.settings_resolver;
export import pi.support.submissions;
export import pi.support.task_scheduler;
export import pi.support.usage_ledger;
export import pi.types.conversation_target;
export import pi.types.harness_options;
export import pi.types.task_inspection;

/**
 * The durable agent harness over one storage backend: a session kernel, the task scheduler, conversations with their
 * inbox and run control, submissions, and the registry/settings/model plumbing that resolves each conversation's agent.
 * Port of packages/durable/src/harness/harness.ts.
 *
 * The registry must hold the built-in tasks (create it with BuiltinTasks().all()). Task scheduling is enabled by the first
 * call that asks for progress (`submit`, `compact`, `abort`, waits) or by `resume()`.
 */
export class Harness : public IConversationHost {
public:
    Harness(std::shared_ptr<IStorage> storage, HarnessOptions options)
        : m_options(std::move(options)), m_storage(storage) {
        m_session = std::make_unique<DurableSession>(
            std::move(storage), [this](Transaction& tx, const Json& record) { return conversationCreated(tx, record); },
            [this] {
                if (m_scheduler) {
                    m_scheduler->join();
                }
            });
        m_scheduler = std::make_unique<TaskScheduler>(*m_session, *m_options.registry, callbacks());
        m_submissions = std::make_unique<Submissions>(
            *m_session, [this] { return now(); }, [this] { return resolvedSettings(); }, [this] { m_scheduler->resume(); });
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    ~Harness() override {
        (void)close();
    }

    /** Opens the harness: checks the registry holds the built-in tasks, then loads live tasks and recovers running ones. */
    Result<void> open() {
        const auto snapshot = m_options.registry->snapshot();
        std::string missing;
        for (const std::string& kind : BuiltinTasks().kinds()) {
            if (!snapshot->task(kind)) {
                missing += (missing.empty() ? "" : ", ") + kind;
            }
        }
        if (!missing.empty()) {
            return std::unexpected(Error{"registry_error", "Registry lacks built-in tasks " + missing + "; create it with BuiltinTasks"});
        }
        if (auto opened = m_scheduler->open(); !opened) {
            (void)close();
            return opened;
        }
        return {};
    }

    /** Seals admission, signals and joins task invocations, and closes storage. Idempotent. */
    Result<void> close() {
        m_closed = true;
        return m_session->close();
    }

    /** Enables task scheduling. Idempotent. */
    Result<void> resume() {
        if (auto open = assertOpen(); !open) {
            return open;
        }
        m_scheduler->resume();
        return {};
    }

    DurableSession& session() {
        return *m_session;
    }

    TaskScheduler& scheduler() {
        return *m_scheduler;
    }

    // ─── Conversations ──────────────────────────────────────────────────────

    /** Returns the reserved root conversation, creating it with `agent` and `init` in one commit when absent. */
    Result<std::shared_ptr<Conversation>> root(const ConversationCreateOptions& options = {}) {
        return create(ConversationTarget{"root", 0, 0}, options);
    }

    /** The conversation with this id; null when it does not exist. */
    Result<std::shared_ptr<Conversation>> conversation(std::int64_t id) {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        std::optional<Json> record;
        auto read = m_session->readOnLine([&]() -> Result<void> {
            auto stored = m_storage->conversation(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            record = *stored;
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        if (!record) {
            return std::shared_ptr<Conversation>();
        }
        return std::make_shared<Conversation>(id, *this);
    }

    Result<std::shared_ptr<Conversation>> createConversation(const ConversationCreateOptions& options = {}) {
        return create(ConversationTarget{"independent", 0, 0}, options);
    }

    // ─── Tasks ──────────────────────────────────────────────────────────────

    Result<std::optional<Json>> getTask(std::int64_t id) {
        std::optional<Json> record;
        auto read = m_session->readOnLine([&]() -> Result<void> {
            auto stored = m_storage->task(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            record = *stored;
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return record;
    }

    /**
     * Live tasks and unsettled submissions: `{scheduling, tasks: [{record, state}], submissions}` where a task's state is
     * `{kind: "running"|"ready"|"waiting"|"completing"|"blocked", ...}` (`migrates`, `on`, `reason`, `error`). Writes nothing
     * and runs no task code.
     */
    Result<Json> inspect() {
        Json report = Json::object();
        auto read = m_session->readOnLine([&]() -> Result<void> {
            auto tasks = m_scheduler->inspect(m_options.registry->snapshot());
            if (!tasks) {
                return std::unexpected(tasks.error());
            }
            Json taskList = Json::array();
            for (const TaskInspection& task : *tasks) {
                taskList.push_back(Json::object({{"record", task.record}, {"state", inspectionState(task)}}));
            }
            std::vector<Json> submissions;
            for (const char* status : {"queued", "placed"}) {
                if (auto scanned = scanSubmissions(status, submissions); !scanned) {
                    return scanned;
                }
            }
            std::sort(submissions.begin(), submissions.end(),
                      [](const Json& a, const Json& b) { return a.at("id").get<std::int64_t>() < b.at("id").get<std::int64_t>(); });
            report = Json::object({{"scheduling", m_scheduler->scheduling()}, {"tasks", taskList}, {"submissions", Json(submissions)}});
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return report;
    }

    /** Reacquires a submission, for example after reopen; null when it does not exist. */
    Result<std::shared_ptr<ISubmission>> submission(std::int64_t id) {
        return m_submissions->get(id);
    }

    /** "aborted", "already_placed", "settled" or "not_found" (unknown, or of another conversation than `conversationId`). */
    Result<std::string> abortSubmission(std::int64_t id, const std::optional<std::int64_t>& conversationId = std::nullopt) {
        return m_submissions->abort(id, conversationId);
    }

    /**
     * Commits the abort mark, signals and joins an active run invocation, and schedules the abort invocation. A task whose
     * definition cannot take it settles as `orphaned` instead. "marked" or "terminal".
     */
    Result<std::string> abortTask(std::int64_t id, const AbortSignal* cancel = nullptr) {
        return m_scheduler->abort(id, cancel);
    }

    /** Blocks until the task is terminal and returns its record; cancelling `cancel` cancels only this wait. */
    Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) {
        m_scheduler->resume();
        return m_scheduler->waitForTask(id, cancel);
    }

    /** Blocks until the ordinary ownership scope of every ownerless conversation has no live non-background task. */
    Result<void> waitForIdle(const AbortSignal* cancel = nullptr) {
        m_scheduler->resume();
        return m_scheduler->waitForIdle(std::nullopt, cancel);
    }

    /** Session total: every conversation's `pi.usage` summed. Each document is read at its own point; totals only grow. */
    Result<Json> usage() {
        std::vector<std::int64_t> ids;
        auto scanned = m_session->readOnLine([&]() -> Result<void> {
            std::optional<Json> cursor;
            do {
                auto page = m_storage->scanConversations(ConversationQuery{}, 256, cursor);
                if (!page) {
                    return std::unexpected(page.error());
                }
                for (const Json& record : page->items) {
                    ids.push_back(record.at("id").get<std::int64_t>());
                }
                cursor = page->next;
            } while (cursor);
            return {};
        });
        if (!scanned) {
            return std::unexpected(scanned.error());
        }
        Json total = m_ledger.empty();
        for (const std::int64_t id : ids) {
            auto state = m_session->snapshot(m_documents.usage(), ownerArgs(id));
            if (!state) {
                return std::unexpected(state.error());
            }
            if (*state) {
                m_ledger.addState(total, **state);
            }
        }
        return total;
    }

    // ─── IConversationHost ──────────────────────────────────────────────────

    std::int64_t now() override {
        if (m_options.now) {
            return m_options.now();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    Result<std::shared_ptr<const IAgent>> agent(std::int64_t conversationId) override {
        return resolveAgent(conversationId, nullptr);
    }

    Result<void> configure(std::int64_t conversationId, const Json& change) override {
        auto committed = m_session->commit([&](Transaction& tx) { return m_configurator.configure(tx, conversationId, change); });
        return committed ? Result<void>() : std::unexpected(committed.error());
    }

    Result<std::shared_ptr<ISubmission>> submit(std::int64_t conversationId, const SubmissionDraft& draft) override {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        return m_submissions->submit(conversationId, draft);
    }

    Result<std::int64_t> compact(std::int64_t conversationId, const std::optional<std::string>& instructions) override {
        m_scheduler->resume();
        Json input = Json::object({{"reason", "manual"}});
        if (instructions) {
            input["instructions"] = *instructions;
        }
        std::int64_t id = 0;
        auto committed = m_session->commit([&](Transaction& tx) -> Result<void> {
            auto created = m_compaction.createCompaction(tx, conversationId, input);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return id;
    }

    Result<std::int64_t> commit(std::int64_t conversationId, const std::function<Result<void>(Transaction&)>& change) override {
        return m_session->commit(change, TransactionScope{conversationId, std::nullopt});
    }

    Result<Json> context(std::int64_t conversationId) override {
        return m_reader.read(*m_session, conversationId, std::nullopt);
    }

    Result<StoragePage> entries(std::int64_t conversationId, const std::optional<std::int64_t>& minEntryId,
                                const std::optional<std::int64_t>& maxEntryId, std::size_t limit,
                                const std::optional<Json>& cursor) override {
        StoragePage page;
        auto read = m_session->readOnLine([&]() -> Result<void> {
            auto scanned = m_storage->scanEntries(EntryQuery{conversationId, minEntryId, maxEntryId}, limit, cursor);
            if (!scanned) {
                return std::unexpected(scanned.error());
            }
            page = std::move(*scanned);
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return page;
    }

    Result<std::int64_t> fork(std::int64_t parentId, std::int64_t at, const ConversationCreateOptions& options) override {
        auto created = create(ConversationTarget{"fork", parentId, at}, options);
        if (!created) {
            return std::unexpected(created.error());
        }
        return (*created)->id();
    }

    Result<void> abort(std::int64_t conversationId, bool background, const AbortSignal* cancel) override {
        m_scheduler->resume();
        return m_scheduler->abortConversation(conversationId, background, cancel);
    }

    Result<void> waitForIdle(std::int64_t conversationId, const AbortSignal* cancel) override {
        m_scheduler->resume();
        return m_scheduler->waitForIdle(conversationId, cancel);
    }

private:
    Result<void> assertOpen() const {
        if (m_closed) {
            return std::unexpected(Error{"harness_closed", "Harness is closed"});
        }
        return {};
    }

    DocAddressArgs ownerArgs(std::int64_t conversationId) const {
        DocAddressArgs args;
        args.owner = conversationId;
        return args;
    }

    ResolvedSettings resolvedSettings() const {
        return m_settingsResolver.resolve(m_options.settings ? m_options.settings() : HarnessRunSettings{});
    }

    void report(const Error& error) const {
        if (m_options.onReport) {
            m_options.onReport(error);
        }
    }

    /** Resolves a conversation's committed `pi.agent` against `snapshot`, or the current registry, and the current settings. */
    Result<std::shared_ptr<const IAgent>> resolveAgent(std::int64_t conversationId, const std::shared_ptr<const IRegistrySnapshot>& snapshot) {
        const std::shared_ptr<const IRegistrySnapshot> registry = snapshot ? snapshot : m_options.registry->snapshot();
        auto state = m_session->snapshot(m_documents.agent(), ownerArgs(conversationId));
        if (!state) {
            return std::unexpected(state.error());
        }
        return std::shared_ptr<const IAgent>(m_agents.resolve(*state, *registry, resolvedSettings(), [this](const Error& error) { report(error); }));
    }

    /** Builds a conversation's environment from its current `cwd`; null without an `env` option. */
    Result<std::shared_ptr<IExecutionEnv>> buildEnv(std::int64_t conversationId) {
        if (!m_options.env) {
            return std::shared_ptr<IExecutionEnv>();
        }
        auto state = m_session->snapshot(m_documents.agent(), ownerArgs(conversationId));
        if (!state) {
            return std::unexpected(state.error());
        }
        std::optional<std::string> cwd;
        if (*state && (*state)->contains("cwd")) {
            cwd = (*state)->at("cwd").get<std::string>();
        }
        return m_options.env(conversationId, cwd);
    }

    SchedulerCallbacks callbacks() {
        SchedulerCallbacks callbacks;
        callbacks.agent = [this](std::int64_t id, const std::shared_ptr<const IRegistrySnapshot>& snapshot) { return resolveAgent(id, snapshot); };
        callbacks.settings = [this] { return resolvedSettings(); };
        callbacks.models = [this] { return m_options.models; };
        callbacks.env = [this](std::int64_t id) { return buildEnv(id); };
        callbacks.now = [this] { return now(); };
        callbacks.report = [this](const Error& error) { report(error); };
        callbacks.settleOutcome = [this](Transaction& tx, const Json& record, const Json& outcome) { return m_settlement.settle(tx, record, outcome); };
        callbacks.withdrawInputs = [this](Transaction& tx, std::int64_t id) { return m_boundary.withdrawQueuedInputs(tx, id); };
        callbacks.conversation = [this](std::int64_t id, const std::shared_ptr<TaskInvocation>& binding) -> Result<std::shared_ptr<IConversationHandle>> {
            std::optional<Json> record;
            auto read = m_session->readOnLine([&]() -> Result<void> {
                auto stored = m_storage->conversation(id);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                record = *stored;
                return {};
            });
            if (!read) {
                return std::unexpected(read.error());
            }
            if (!record) {
                return std::shared_ptr<IConversationHandle>();
            }
            return std::shared_ptr<IConversationHandle>(std::make_shared<BoundConversation>(*this, id, binding));
        };
        callbacks.context = [this](std::int64_t id, const std::optional<std::int64_t>& at) { return m_reader.read(*m_session, id, at); };
        return callbacks;
    }

    /**
     * The built-in creation hook, in every commit that creates or forks a conversation: empty `pi.live`, `pi.inbox` and
     * `pi.usage`, the conversation's `pi.agent`, then `HarnessOptions.conversationCreated`.
     */
    Result<void> conversationCreated(Transaction& tx, const Json& record) {
        const DocAddressArgs args = ownerArgs(record.at("id").get<std::int64_t>());
        for (const DocDefinition& definition : {m_documents.live(), m_documents.inbox(), m_documents.usage()}) {
            if (auto doc = tx.doc(definition, args); !doc) {
                return std::unexpected(doc.error());
            }
        }
        if (auto agent = m_configurator.createAgent(tx, record); !agent) {
            return agent;
        }
        return m_options.conversationCreated ? m_options.conversationCreated(tx, record) : Result<void>();
    }

    Result<std::shared_ptr<Conversation>> create(const ConversationTarget& target, const ConversationCreateOptions& options) {
        if (auto open = assertOpen(); !open) {
            return std::unexpected(open.error());
        }
        std::int64_t id = 0;
        auto committed = m_session->commit([&](Transaction& tx) -> Result<void> {
            if (target.kind == "root") {
                auto existing = tx.conversation(Transaction::kRootConversationId);
                if (!existing) {
                    return std::unexpected(existing.error());
                }
                if (*existing) {
                    id = Transaction::kRootConversationId;
                    return {};
                }
            }
            Result<Json> record = target.kind == "root"   ? tx.createRootConversation()
                                  : target.kind == "fork" ? tx.forkConversation(target.parent, target.at, options.ownership)
                                                          : tx.createConversation(options.ownership);
            if (!record) {
                return std::unexpected(record.error());
            }
            id = record->at("id").get<std::int64_t>();
            if (options.agent) {
                if (auto configured = m_configurator.configure(tx, id, *options.agent); !configured) {
                    return configured;
                }
            }
            return options.init ? options.init(tx, id) : Result<void>();
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return std::make_shared<Conversation>(id, *this);
    }

    Result<void> scanSubmissions(const char* status, std::vector<Json>& into) {
        std::optional<Json> cursor;
        do {
            SubmissionQuery query;
            query.status = status;
            auto page = m_storage->scanSubmissions(query, 256, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            into.insert(into.end(), page->items.begin(), page->items.end());
            cursor = page->next;
        } while (cursor);
        return {};
    }

    Json inspectionState(const TaskInspection& task) const {
        Json state = Json::object({{"kind", task.state}});
        if (task.state == "ready") {
            state["migrates"] = task.migrates;
        } else if (task.state == "waiting") {
            state["on"] = Json(task.on);
        } else if (task.state == "blocked") {
            state["reason"] = task.reason;
            if (task.error) {
                state["error"] = *task.error;
            }
        }
        return state;
    }

    HarnessOptions m_options;
    std::shared_ptr<IStorage> m_storage;
    BuiltinDocuments m_documents;
    SettingsResolver m_settingsResolver;
    AgentResolver m_agents;
    AgentConfigurator m_configurator;
    CompactionPlanner m_compaction;
    ContextReader m_reader;
    InboxBoundary m_boundary;
    OutcomeSettlement m_settlement;
    UsageLedger m_ledger;
    std::atomic<bool> m_closed{false};
    // Destroyed in reverse order: submissions and the scheduler unsubscribe from the session first.
    std::unique_ptr<DurableSession> m_session;
    std::unique_ptr<TaskScheduler> m_scheduler;
    std::unique_ptr<Submissions> m_submissions;
};
