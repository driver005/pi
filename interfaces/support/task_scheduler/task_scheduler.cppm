module;

#include <cstdint>

export module pi.support.task_scheduler;

import std;
export import pi.durable.i_registry_reader;
export import pi.durable.i_storage;
export import pi.durable.i_task_host;
export import pi.support.abort_link;
export import pi.support.durable_session;
export import pi.support.invocation_runtime;
export import pi.support.json_equality;
export import pi.support.json_waiters;
export import pi.support.task_records;
export import pi.support.wait_gate;
export import pi.types.idle_scope;
export import pi.types.ownership_overlay;
export import pi.types.ownership_ref;
export import pi.types.ownership_step;
export import pi.types.scheduler_callbacks;
export import pi.types.step_decision;
export import pi.types.task_inspection;
export import pi.types.task_invocation;
export import pi.types.task_node;
export import pi.types.task_reservation;
export import pi.types.task_resolution;

/**
 * Durable task scheduler of one harness. Port of packages/durable/src/harness/scheduler.ts.
 *
 * `m_live` mirrors every committed non-terminal task record (pending, running, waiting, completing). The line
 * listener updates it on the session line, so code running on the line reads exactly the committed state from it.
 * All ownership state is protected by that line; the wake flags and the invocation table have their own locks.
 *
 * Tasks and conversations form one ownership tree: a task's parent is its owner task, or its conversation; a
 * conversation's parent is its owner task, if any. Walks up that tree decide cascades, idle scopes, and whether a
 * task's ordinary owned work is live, which holds its outcome as `completing` and delays its abort handler.
 *
 * Every task transition is decided and written by one callback serialized on the session line. That covers
 * reservation, marks, runtime commits, finalization, and the synchronous step before each phase, which applies the
 * precedence rules and writes a fault or handover. Handlers and joins run off the line, on one thread per
 * invocation; one worker thread reconciles and reserves.
 */
export class TaskScheduler : public ITaskHost {
public:
    static constexpr std::size_t kScanPageSize = 256;
    static constexpr std::int64_t kRootsKey = -1;

    TaskScheduler(DurableSession& session, IRegistryReader& registry, SchedulerCallbacks callbacks)
        : m_session(session), m_registry(registry), m_callbacks(std::move(callbacks)) {}

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    ~TaskScheduler() override {
        join();
        m_registry.unsubscribe(m_registrySubscription);
        if (m_lineSubscription != 0) {
            m_session.unsubscribeCommitsOnLine(m_lineSubscription);
        }
        if (m_closeSubscription != 0) {
            m_session.unsubscribeClose(m_closeSubscription);
        }
    }

    /** Loads live tasks and changes surviving `running` tasks back to `pending`. Dispatches nothing. */
    Result<void> open() {
        auto line = m_session.subscribeCommitsOnLine([this](const Json& publication) { observe(publication); });
        if (!line) {
            return std::unexpected(line.error());
        }
        m_lineSubscription = *line;
        auto close = m_session.subscribeClose([this] { seal(); });
        if (!close) {
            return std::unexpected(close.error());
        }
        m_closeSubscription = *close;
        m_registrySubscription = m_registry.subscribe([this] { kick(); });
        m_worker = std::thread([this] { workerLoop(); });
        auto committed = m_session.commit([this](Transaction& tx) { return loadLive(tx); });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        // Derive abort marks a crash left unapplied below cancelled owners, and finalize held outcomes.
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_cascadePending = true;
        }
        scheduleReconcile();
        return {};
    }

    /** Enables scheduling. Idempotent; the kick does nothing once closing. */
    void resume() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_enabled = true;
        }
        kick();
    }

    /** Waits for every invocation signalled by the close listener and stops the worker. Writes nothing. */
    void join() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::vector<std::thread> threads;
        {
            const std::lock_guard<std::mutex> lock(m_threadMutex);
            threads.swap(m_threads);
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    /**
     * Commits the abort mark, or settles a task that no registered definition can take as `orphaned` when nothing it
     * owns is live, then joins the run invocation seen on the line; the line listener signalled it. The abort
     * invocation starts once the task's ordinary owned work is gone. A `completing` task is only marked.
     */
    Result<std::string> abort(std::int64_t id, const AbortSignal* cancel = nullptr) {
        std::string result = "marked";
        std::shared_ptr<TaskInvocation> run;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto current = tx.task(id);
            if (!current) {
                return std::unexpected(current.error());
            }
            if (!*current) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(id) + " does not exist"});
            }
            const Json& record = **current;
            if (record.at("state").at("status") == "terminal") {
                result = "terminal";
                return {};
            }
            std::shared_ptr<TaskInvocation> invocation = invocationOf(id);
            if (!invocation && record.at("state").at("status") != "completing") {
                if (auto loaded = loadScopes(false); !loaded) {
                    return std::unexpected(loaded.error());
                }
                if (!ownedLive(nullptr).contains(id)) {
                    TaskResolution resolution = resolve(record, m_registry.snapshot());
                    if (!resolution.ready) {
                        return terminate(tx, record, Json::object({{"status", "orphaned"}, {"reason", resolution.reason}}));
                    }
                }
            }
            if (!record.value("abortRequested", false)) {
                Json marked = record;
                marked["abortRequested"] = true;
                if (auto set = tx.setTask(marked); !set) {
                    return set;
                }
            }
            if (invocation && invocation->mode == "run") {
                run = invocation;
            }
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        // The line listener signalled the run; join it.
        if (run) {
            if (auto waited = run->done.wait({cancel}); !waited) {
                return std::unexpected(waited.error());
            }
        }
        return result;
    }

    /** Blocks until the task is terminal and returns its record. */
    Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) {
        std::shared_ptr<WaiterSlot> slot;
        std::optional<Json> record;
        // Check and register on the line so no terminal publication falls between them.
        auto read = m_session.readOnLine([&]() -> Result<void> {
            if (closing()) {
                return std::unexpected(closedError());
            }
            if (m_live.contains(id)) {
                slot = m_taskWaiters.add(id, cancel);
                return {};
            }
            auto stored = storage().task(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            if (!*stored) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(id) + " does not exist"});
            }
            record = **stored;
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        if (slot) {
            return m_taskWaiters.await(slot);
        }
        return *record;
    }

    /** Blocks until ordinary traversal from the conversation, or every ownerless conversation, reaches no live non-background task. */
    Result<void> waitForIdle(const std::optional<std::int64_t>& conversationId, const AbortSignal* cancel = nullptr) {
        std::shared_ptr<WaiterSlot> slot;
        auto registered = m_session.readOnLine([&]() -> Result<void> {
            if (closing()) {
                return std::unexpected(closedError());
            }
            if (idle(conversationId)) {
                return {};
            }
            scheduleReconcile();
            slot = m_idleWaiters.add(conversationId.value_or(kRootsKey), cancel);
            return {};
        });
        if (!registered) {
            return std::unexpected(registered.error());
        }
        if (!slot) {
            return {};
        }
        auto waited = m_idleWaiters.await(slot);
        return waited ? Result<void>() : std::unexpected(waited.error());
    }

    /**
     * Conversation abort: in one commit, withdraws the queued inputs and marks every live non-background task that
     * ordinary traversal from the conversation reaches; resolves once the scope is idle. With `background`, traversal
     * crosses background boundaries, and the wait also covers every task it reached.
     */
    Result<void> abortConversation(std::int64_t conversationId, bool background, const AbortSignal* cancel = nullptr) {
        std::vector<std::int64_t> reached;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto queued = loadScopes(true);
            if (!queued) {
                return std::unexpected(queued.error());
            }
            const IdleScope scope{false, conversationId};
            for (const auto& item : m_live) {
                const Json& record = item.second;
                if (record.value("background", false) && !background) {
                    continue;
                }
                if (inScope(m_records.parentOf(record), scope, background) != std::optional<bool>(true)) {
                    continue;
                }
                reached.push_back(item.first);
                if (!record.value("abortRequested", false)) {
                    Json marked = record;
                    marked["abortRequested"] = true;
                    if (auto set = tx.setTask(marked); !set) {
                        return set;
                    }
                }
            }
            for (const std::int64_t id : *queued) {
                if (inScope(OwnershipRef{false, id}, scope, background) == std::optional<bool>(true)) {
                    if (auto withdrawn = m_callbacks.withdrawInputs(tx, id); !withdrawn) {
                        return withdrawn;
                    }
                }
            }
            return {};
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        if (background) {
            for (const std::int64_t id : reached) {
                if (auto waited = waitForTask(id, cancel); !waited) {
                    return std::unexpected(waited.error());
                }
            }
        }
        return waitForIdle(conversationId, cancel);
    }

    /**
     * Scheduling state and every live task with its derived state; call on the session line. Runs no task code: a
     * pending migration shows as `ready` with `migrates`, and only a migration the scheduler already tried, or one
     * that cannot exist, shows as failed.
     */
    Result<std::vector<TaskInspection>> inspect(const std::shared_ptr<const IRegistrySnapshot>& snapshot) {
        if (auto loaded = loadScopes(false); !loaded) {
            return std::unexpected(loaded.error());
        }
        const auto owned = ownedLive(nullptr);
        std::vector<TaskInspection> tasks;
        for (const auto& item : m_live) {
            tasks.push_back(inspectTask(item.second, snapshot, owned));
        }
        return tasks;
    }

    /** "paused", "running" or "closing". */
    std::string scheduling() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closing ? "closing" : (m_enabled ? "running" : "paused");
    }

    // ─── ITaskHost: the services of one invocation's runtime ────────────────

    Result<void> commit(TaskInvocation& invocation,
                        const std::function<Result<std::optional<Json>>(Transaction&, const Json&)>& change) override {
        return gated(invocation, [&](Transaction& tx, const Json& current) -> Result<void> {
            auto next = change(tx, current);
            if (!next) {
                return std::unexpected(next.error());
            }
            if (!*next) {
                return {};
            }
            return commitState(tx, invocation, current, **next);
        });
    }

    Result<std::optional<Json>> memo(TaskInvocation& invocation, const std::string& name) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        std::optional<Json> value;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto found = m_live.find(invocation.taskId);
            if (found != m_live.end()) {
                value = m_records.memoOf(found->second, name);
            }
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return value;
    }

    Result<Json> memo(TaskInvocation& invocation, const std::string& name, const Json& candidate) override {
        Json winner = candidate;
        auto committed = gated(invocation, [&](Transaction& tx, const Json& current) -> Result<void> {
            if (auto existing = m_records.memoOf(current, name)) {
                winner = *existing;
                return {};
            }
            Json next = current;
            if (!next.contains("memos")) {
                next["memos"] = Json::object();
            }
            next["memos"][name] = candidate;
            return tx.setTask(next);
        });
        if (!committed) {
            return std::unexpected(committed.error());
        }
        return winner;
    }

    Result<std::optional<Json>> snapshot(TaskInvocation& invocation, const DocDefinition& definition,
                                         const DocAddressArgs& args) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_session.snapshot(definition, args);
    }

    Result<std::optional<Json>> snapshotAsOf(TaskInvocation& invocation, const DocDefinition& definition,
                                             const DocAddressArgs& args, std::int64_t entryId) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_session.snapshotAsOf(definition, args, entryId);
    }

    Result<std::optional<Json>> getTask(TaskInvocation& invocation, std::int64_t id) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        std::optional<Json> record;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto stored = storage().task(id);
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

    Result<Json> waitForTask(TaskInvocation& invocation, std::int64_t id, const AbortSignal* cancel) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        AbortLink link({&invocation.signal, cancel});
        return waitForTask(id, link.signal());
    }

    Result<std::vector<Json>> outcomes(TaskInvocation& invocation, const std::vector<std::int64_t>& ids) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        std::vector<Json> outcomes;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            for (const std::int64_t id : ids) {
                auto stored = storage().task(id);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                if (!*stored || (*stored)->at("state").at("status") != "terminal") {
                    return std::unexpected(Error{"durable_error", "Task " + std::to_string(id) + " is not terminal"});
                }
                outcomes.push_back((*stored)->at("state").at("outcome"));
            }
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return outcomes;
    }

    Result<std::shared_ptr<IConversationHandle>> conversation(const std::shared_ptr<TaskInvocation>& invocation, std::int64_t id) override {
        if (auto live = checkLive(*invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_callbacks.conversation(id, invocation);
    }

    Result<std::optional<Json>> entry(TaskInvocation& invocation, std::int64_t id,
                                      const std::optional<std::string>& kind) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        std::optional<Json> record;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto found = storage().visibleEntry(invocation.conversationId, id);
            if (!found) {
                return std::unexpected(found.error());
            }
            if (*found && (!kind || (*found)->entry.value("kind", std::string()) == *kind)) {
                record = (*found)->entry;
            }
            return {};
        });
        if (!read) {
            return std::unexpected(read.error());
        }
        return record;
    }

    Result<Json> context(TaskInvocation& invocation, std::int64_t conversationId,
                         const std::optional<std::int64_t>& at) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_callbacks.context(conversationId, at);
    }

    Result<std::shared_ptr<const IAgent>> resolveAgent(TaskInvocation& invocation,
                                                       const std::shared_ptr<const IRegistrySnapshot>& snapshot) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_callbacks.agent(invocation.conversationId, snapshot);
    }

    ResolvedSettings settings() override {
        return m_callbacks.settings();
    }

    IModelRuntime* models() override {
        return m_callbacks.models ? m_callbacks.models() : nullptr;
    }

    Result<std::shared_ptr<IExecutionEnv>> env(TaskInvocation& invocation) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        return m_callbacks.env(invocation.conversationId);
    }

    std::int64_t now(TaskInvocation&) override {
        return m_callbacks.now();
    }

    void report(TaskInvocation&, const Error& error) override {
        m_callbacks.report(error);
    }

    /** Waits until the harness clock reaches `until`, rechecking it after every short wait. */
    Result<void> sleep(TaskInvocation& invocation, std::int64_t until, const AbortSignal* cancel) override {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        const WaitGate never;
        while (true) {
            const std::int64_t remaining = until - m_callbacks.now();
            if (remaining <= 0) {
                return {};
            }
            auto waited = never.waitFor(std::chrono::milliseconds(std::min<std::int64_t>(remaining, kSleepSliceMs)),
                                        {&invocation.signal, cancel});
            if (!waited) {
                return std::unexpected(waited.error());
            }
        }
    }

private:
    static constexpr std::int64_t kSleepSliceMs = 25;

    // ─── Wiring ─────────────────────────────────────────────────────────────

    IStorage& storage() {
        return m_session.storage();
    }

    Error closedError() const {
        return Error{"harness_closed", "Harness is closed"};
    }

    bool closing() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closing;
    }

    bool enabled() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_enabled;
    }

    std::shared_ptr<TaskInvocation> invocationOf(std::int64_t id) {
        const std::lock_guard<std::mutex> lock(m_invocationMutex);
        auto found = m_invocations.find(id);
        return found == m_invocations.end() ? nullptr : found->second;
    }

    Result<void> checkLive(const TaskInvocation& invocation) const {
        if (invocation.ended) {
            return std::unexpected(endedError(invocation));
        }
        return {};
    }

    Error endedError(const TaskInvocation& invocation) const {
        return Error{"invocation_ended", "Task " + std::to_string(invocation.taskId) + " invocation has ended"};
    }

    /** Close listener: runs synchronously once admission is sealed, before `join()`. */
    void seal() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_closing = true;
        }
        m_registry.unsubscribe(m_registrySubscription);
        const Error error = closedError();
        m_taskWaiters.rejectAll(error);
        m_idleWaiters.rejectAll(error);
        std::vector<std::shared_ptr<TaskInvocation>> invocations;
        {
            const std::lock_guard<std::mutex> lock(m_invocationMutex);
            for (const auto& item : m_invocations) {
                invocations.push_back(item.second);
            }
        }
        for (const auto& invocation : invocations) {
            invocation->signal.abort();
        }
        m_cv.notify_all();
    }

    void kick() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_dirty = true;
        }
        m_cv.notify_all();
    }

    void scheduleReconcile() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closing) {
                return;
            }
            m_reconcileRequested = true;
        }
        m_cv.notify_all();
    }

    void markCascadePending() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_cascadePending = true;
    }

    void addFailFastCheck(std::int64_t id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_failFastChecks.insert(id);
    }

    // ─── Worker ─────────────────────────────────────────────────────────────

    void workerLoop() {
        while (true) {
            bool reconcileNow = false;
            bool drainNow = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || m_reconcileRequested || (m_dirty && m_enabled && !m_closing); });
                if (m_stop) {
                    return;
                }
                reconcileNow = m_reconcileRequested;
                m_reconcileRequested = false;
                drainNow = m_dirty && m_enabled && !m_closing;
                if (drainNow) {
                    m_dirty = false;
                }
            }
            if (reconcileNow) {
                reconcile();
            }
            if (drainNow) {
                drain();
            }
        }
    }

    /** Reserves every eligible task and starts its invocation; a failure is reported and the pass retried on the next kick. */
    void drain() {
        auto reservations = reserve();
        if (!reservations) {
            if (!closing()) {
                m_callbacks.report(reservations.error());
            }
            return;
        }
        for (TaskReservation& reservation : *reservations) {
            start(std::move(reservation));
        }
    }

    // ─── Opening ────────────────────────────────────────────────────────────

    Result<std::vector<Json>> scanLive(Transaction& tx, const std::string& status) {
        std::vector<Json> records;
        std::optional<Json> cursor;
        do {
            TaskQuery query;
            query.status = status;
            auto page = tx.scanTasks(query, kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            records.insert(records.end(), page->items.begin(), page->items.end());
            cursor = page->next;
        } while (cursor);
        return records;
    }

    Result<void> loadLive(Transaction& tx) {
        // Every table read before the first write.
        std::vector<std::vector<Json>> scans;
        for (const char* status : {"pending", "running", "waiting", "completing"}) {
            auto records = scanLive(tx, status);
            if (!records) {
                return std::unexpected(records.error());
            }
            scans.push_back(std::move(*records));
        }
        for (const auto& records : scans) {
            for (const Json& record : records) {
                const std::int64_t id = record.at("id").get<std::int64_t>();
                m_live[id] = record;
                const Json& state = record.at("state");
                if (state.at("status") == "running") {
                    Json pending = Json::object({{"status", "pending"}, {"checkpoint", state.at("checkpoint")}});
                    if (auto set = tx.setTask(m_records.withState(record, pending)); !set) {
                        return set;
                    }
                }
                if (state.at("status") == "waiting" && state.at("policy") == "failFast") {
                    addFailFastCheck(id);
                }
            }
        }
        return {};
    }

    // ─── Scheduling ─────────────────────────────────────────────────────────

    /** Line listener: mirrors committed task, conversation and submission changes. */
    void observe(const Json& publication) {
        std::vector<Json> updated;
        std::vector<std::int64_t> failed;
        bool changed = false;
        for (const Json& change : publication.at("changes")) {
            if (change.at("type") != "task") {
                continue;
            }
            changed = true;
            observeTask(change.at("value"), updated, failed);
        }
        for (const std::int64_t id : failed) {
            for (const auto& item : m_live) {
                const Json& state = item.second.at("state");
                if (state.at("status") == "waiting" && state.at("policy") == "failFast" && containsId(state.at("on"), id)) {
                    addFailFastCheck(item.first);
                    scheduleReconcile();
                }
            }
        }
        for (const Json& change : publication.at("changes")) {
            if (change.at("type") == "conversation" && !m_edges.contains(change.at("value").at("id").get<std::int64_t>())) {
                const Json& value = change.at("value");
                setEdge(value.at("id").get<std::int64_t>(),
                        value.contains("owner") ? std::optional<std::int64_t>(value.at("owner").at("taskId").get<std::int64_t>())
                                                : std::nullopt);
            }
        }
        for (const Json& change : publication.at("changes")) {
            // A queued input below a cancelled owner is withdrawn, even after its cascade.
            if (change.at("type") != "submission") {
                continue;
            }
            const Json& value = change.at("value");
            if (value.at("status") != "queued" || value.at("type") != "input") {
                continue;
            }
            const OwnershipRef up{false, value.at("conversationId").get<std::int64_t>()};
            if (!chainKnown(up, nullptr) || belowCancelled(up)) {
                markCascadePending();
            }
        }
        for (const Json& record : updated) {
            // Work created below a cancelled owner, even after its cascade, is aborted too.
            const OwnershipRef parent = m_records.parentOf(record);
            if (!chainKnown(parent, nullptr)) {
                scheduleReconcile();
            } else if (!record.value("background", false) && !record.value("abortRequested", false) && belowCancelled(parent)) {
                markCascadePending();
            }
        }
        // Also retries, with the next commit of any kind, a cascade whose commit failed.
        if (cascadePending()) {
            scheduleReconcile();
        }
        if (!changed) {
            return;
        }
        resolveIdleWaiters();
        kick();
    }

    void observeTask(const Json& record, std::vector<Json>& updated, std::vector<std::int64_t>& failed) {
        const std::int64_t id = record.at("id").get<std::int64_t>();
        auto previousFound = m_live.find(id);
        const std::optional<Json> previous = previousFound == m_live.end() ? std::nullopt : std::optional<Json>(previousFound->second);
        if (m_records.failedOutcome(record) && (!previous || !m_records.failedOutcome(*previous))) {
            failed.push_back(id);
        }
        const std::string status = record.at("state").at("status").get<std::string>();
        if (status == "terminal") {
            m_live.erase(id);
            m_failedMigrations.erase(id);
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_failFastChecks.erase(id);
            }
            if (m_conversationOwners.contains(id)) {
                m_settled[id] = m_records.nodeOf(record);
            }
            m_taskWaiters.resolve(id, record);
            // Its owner may finalize now.
            scheduleReconcile();
            return;
        }
        if (record.value("abortRequested", false) && !(previous && previous->value("abortRequested", false))) {
            markCascadePending();
            // Signal a run invocation of the newly marked task; its next step ends it.
            if (auto invocation = invocationOf(id); invocation && invocation->mode == "run") {
                invocation->signal.abort();
            }
        }
        if (status == "completing" && !(previous && previous->at("state").at("status") == "completing")) {
            if (m_records.cancellationIntent(record)) {
                markCascadePending();
            }
            scheduleReconcile();
        }
        if (status == "waiting" && record.at("state").at("policy") == "failFast" &&
            !(previous && previous->at("state").at("status") == "waiting")) {
            addFailFastCheck(id);
            scheduleReconcile();
        }
        m_live[id] = record;
        updated.push_back(record);
    }

    bool cascadePending() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_cascadePending;
    }

    bool containsId(const Json& ids, std::int64_t id) const {
        for (const Json& item : ids) {
            if (item.get<std::int64_t>() == id) {
                return true;
            }
        }
        return false;
    }

    void resolveIdleWaiters() {
        for (const std::int64_t key : m_idleWaiters.keys()) {
            if (idle(key == kRootsKey ? std::nullopt : std::optional<std::int64_t>(key))) {
                m_idleWaiters.resolve(key, Json(nullptr));
            }
        }
    }

    // ─── Reconcile ──────────────────────────────────────────────────────────

    /**
     * One commit that applies what committed records imply: abort marks below live owners with cancellation intent,
     * `failFast` marks, withdrawn queued inputs below cancelled owners, and the final terminal record of every
     * `completing` task whose ordinary owned work is gone. The durable records are the intent, so this also repairs
     * whatever a crash left unapplied. Resolves idle waiters that the loaded edges decide.
     */
    void reconcile() {
        bool cascade = false;
        std::vector<std::int64_t> checks;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            cascade = m_cascadePending;
            m_cascadePending = false;
            checks.assign(m_failFastChecks.begin(), m_failFastChecks.end());
            m_failFastChecks.clear();
        }
        auto committed = m_session.commit([&](Transaction& tx) { return reconcileInTransaction(tx, cascade, checks); });
        if (!committed) {
            // Any pass may have staged marks, so a failed one is retried with the next commit.
            markCascadePending();
            for (const std::int64_t id : checks) {
                addFailFastCheck(id);
            }
            if (!closing()) {
                m_callbacks.report(committed.error());
            }
        }
        m_session.readOnLine([&]() -> Result<void> {
            resolveIdleWaiters();
            return {};
        });
    }

    Result<void> reconcileInTransaction(Transaction& tx, bool cascade, const std::vector<std::int64_t>& checks) {
        if (closing()) {
            return {};
        }
        auto queued = loadScopes(cascade);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        std::set<std::int64_t> marked;
        const auto mark = [&](const Json& record) -> Result<void> {
            const std::int64_t id = record.at("id").get<std::int64_t>();
            if (record.value("abortRequested", false) || !marked.insert(id).second) {
                return {};
            }
            Json next = record;
            next["abortRequested"] = true;
            return tx.setTask(next);
        };
        // Loading edges can reveal a cancelled owner, so marks are derived on every pass.
        for (const auto& item : m_live) {
            if (!item.second.value("background", false) && belowCancelled(m_records.parentOf(item.second))) {
                if (auto set = mark(item.second); !set) {
                    return set;
                }
            }
        }
        for (const std::int64_t id : checks) {
            if (auto failFast = markFailFast(id, mark); !failFast) {
                return failFast;
            }
        }
        for (const std::int64_t id : *queued) {
            if (belowCancelled(OwnershipRef{false, id})) {
                if (auto withdrawn = m_callbacks.withdrawInputs(tx, id); !withdrawn) {
                    return withdrawn;
                }
            }
        }
        return finalize(tx);
    }

    Result<void> markFailFast(std::int64_t id, const std::function<Result<void>(const Json&)>& mark) {
        auto waiter = m_live.find(id);
        if (waiter == m_live.end() || waiter->second.at("state").at("status") != "waiting") {
            return {};
        }
        const Json on = waiter->second.at("state").at("on");
        auto failed = anyFailed(on);
        if (!failed) {
            return std::unexpected(failed.error());
        }
        if (!*failed) {
            return {};
        }
        // Every other live task: the failed one keeps its own outcome.
        for (const Json& member : on) {
            auto record = m_live.find(member.get<std::int64_t>());
            if (record != m_live.end() && !m_records.failedOutcome(record->second)) {
                if (auto marked = mark(record->second); !marked) {
                    return marked;
                }
            }
        }
        return {};
    }

    /** Whether any of `ids` holds or ended with an outcome other than `completed`. */
    Result<bool> anyFailed(const Json& ids) {
        for (const Json& item : ids) {
            const std::int64_t id = item.get<std::int64_t>();
            auto live = m_live.find(id);
            if (live != m_live.end()) {
                if (m_records.failedOutcome(live->second)) {
                    return true;
                }
                continue;
            }
            auto stored = storage().task(id);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            if (*stored && m_records.failedOutcome(**stored)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Writes the terminal record of every `completing` task without live ordinary owned work. Finalizing one can free
     * its owner, so this repeats over the commit's candidates until nothing changes. A held scheduler outcome gets its
     * harness cleanup here.
     */
    Result<void> finalize(Transaction& tx) {
        while (true) {
            const OwnershipOverlay overlay = overlayOf(tx);
            const auto owned = ownedLive(&overlay);
            std::vector<Json> done;
            for (const Json& record : liveRecords(&overlay)) {
                if (record.at("state").at("status") == "completing" &&
                    !owned.contains(record.at("id").get<std::int64_t>())) {
                    done.push_back(record);
                }
            }
            if (done.empty()) {
                return {};
            }
            for (const Json& record : done) {
                const Json outcome = record.at("state").at("outcome");
                Json terminal = Json::object({{"status", "terminal"}, {"outcome", outcome}});
                if (auto set = tx.setTask(m_records.withState(record, terminal)); !set) {
                    return set;
                }
                // Only the scheduler writes `faulted` and `orphaned`; their cleanup waits for this commit.
                const Json status = outcome.at("status");
                if (status == "faulted" || status == "orphaned") {
                    if (auto settled = m_callbacks.settleOutcome(tx, record, outcome); !settled) {
                        return settled;
                    }
                }
            }
        }
    }

    // ─── Ownership ──────────────────────────────────────────────────────────

    /**
     * Loads the owner chains of every live task and, with `queued`, of every conversation with queued submissions, on
     * the session line; returns the latter. Reads committed storage directly, so it may run inside a commit callback.
     */
    Result<std::vector<std::int64_t>> loadScopes(bool queued) {
        std::vector<Json> live;
        for (const auto& item : m_live) {
            live.push_back(item.second);
        }
        for (const Json& record : live) {
            const OwnershipRef parent = m_records.parentOf(record);
            if (!chainKnown(parent, nullptr)) {
                if (auto loaded = loadChain(parent, nullptr); !loaded) {
                    return std::unexpected(loaded.error());
                }
            }
        }
        std::vector<std::int64_t> conversations;
        if (!queued) {
            return conversations;
        }
        std::set<std::int64_t> seen;
        std::optional<Json> cursor;
        do {
            SubmissionQuery query;
            query.status = "queued";
            auto page = storage().scanSubmissions(query, kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            for (const Json& submission : page->items) {
                const std::int64_t id = submission.at("conversationId").get<std::int64_t>();
                if (seen.insert(id).second) {
                    conversations.push_back(id);
                }
            }
            cursor = page->next;
        } while (cursor);
        for (const std::int64_t id : conversations) {
            if (auto loaded = loadChain(OwnershipRef{false, id}, nullptr); !loaded) {
                return std::unexpected(loaded.error());
            }
        }
        return conversations;
    }

    /** Loads the owner edges and task nodes from `start` up to its ownerless root. */
    Result<void> loadChain(const OwnershipRef& start, const OwnershipOverlay* overlay) {
        std::optional<OwnershipRef> at = start;
        while (at) {
            if (at->task) {
                std::optional<TaskNode> node = nodeOf(at->id, overlay);
                if (!node) {
                    auto record = storage().task(at->id);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    if (!*record) {
                        return {};
                    }
                    node = m_records.nodeOf(**record);
                    if ((*record)->at("state").at("status") == "terminal") {
                        m_settled[at->id] = *node;
                    }
                }
                at = m_records.parentOf(*node);
            } else {
                auto edge = edgeOf(at->id, overlay);
                std::optional<std::int64_t> owner;
                if (!edge) {
                    auto record = storage().conversation(at->id);
                    if (!record) {
                        return std::unexpected(record.error());
                    }
                    if (*record && (*record)->contains("owner")) {
                        owner = (*record)->at("owner").at("taskId").get<std::int64_t>();
                    }
                    setEdge(at->id, owner);
                } else {
                    owner = *edge;
                }
                at = owner ? std::optional<OwnershipRef>(OwnershipRef{true, *owner}) : std::nullopt;
            }
        }
        return {};
    }

    void setEdge(std::int64_t conversationId, const std::optional<std::int64_t>& owner) {
        m_edges[conversationId] = owner;
        if (owner) {
            m_conversationOwners.insert(*owner);
        }
    }

    /** The owner task of a conversation: outer nullopt while not loaded, inner nullopt when ownerless. */
    std::optional<std::optional<std::int64_t>> edgeOf(std::int64_t id, const OwnershipOverlay* overlay) const {
        if (overlay != nullptr) {
            auto staged = overlay->edges.find(id);
            if (staged != overlay->edges.end()) {
                return staged->second;
            }
        }
        auto found = m_edges.find(id);
        if (found == m_edges.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    std::optional<TaskNode> nodeOf(std::int64_t id, const OwnershipOverlay* overlay) const {
        if (overlay != nullptr) {
            auto staged = overlay->tasks.find(id);
            if (staged != overlay->tasks.end()) {
                return m_records.nodeOf(staged->second);
            }
        }
        if (auto live = m_live.find(id); live != m_live.end()) {
            return m_records.nodeOf(live->second);
        }
        if (auto settled = m_settled.find(id); settled != m_settled.end()) {
            return settled->second;
        }
        return std::nullopt;
    }

    /** Walks up from `start`: owner tasks and conversations, ending at an ownerless root or an edge not loaded yet. */
    std::vector<OwnershipStep> above(const OwnershipRef& start, const OwnershipOverlay* overlay) const {
        std::vector<OwnershipStep> steps;
        std::optional<OwnershipRef> at = start;
        while (at && steps.size() < kMaxDepth) {
            if (at->task) {
                auto node = nodeOf(at->id, overlay);
                if (!node) {
                    steps.push_back(OwnershipStep{"unknown", 0, TaskNode{}});
                    return steps;
                }
                steps.push_back(OwnershipStep{"task", at->id, *node});
                at = m_records.parentOf(*node);
            } else {
                steps.push_back(OwnershipStep{"conversation", at->id, TaskNode{}});
                auto edge = edgeOf(at->id, overlay);
                if (!edge) {
                    steps.push_back(OwnershipStep{"unknown", 0, TaskNode{}});
                    return steps;
                }
                at = *edge ? std::optional<OwnershipRef>(OwnershipRef{true, **edge}) : std::nullopt;
            }
        }
        return steps;
    }

    /** Whether every owner above `start` is loaded. */
    bool chainKnown(const OwnershipRef& start, const OwnershipOverlay* overlay) const {
        for (const OwnershipStep& step : above(start, overlay)) {
            if (step.kind == "unknown") {
                return false;
            }
        }
        return true;
    }

    /** Live records, with the overlay's candidates replacing committed ones; terminal candidates are gone. */
    std::vector<Json> liveRecords(const OwnershipOverlay* overlay) const {
        std::vector<Json> records;
        for (const auto& item : m_live) {
            const Json* candidate = &item.second;
            if (overlay != nullptr) {
                auto staged = overlay->tasks.find(item.first);
                if (staged != overlay->tasks.end()) {
                    candidate = &staged->second;
                }
            }
            if (candidate->at("state").at("status") != "terminal") {
                records.push_back(*candidate);
            }
        }
        if (overlay != nullptr) {
            for (const auto& item : overlay->tasks) {
                if (!m_live.contains(item.first) && item.second.at("state").at("status") != "terminal") {
                    records.push_back(item.second);
                }
            }
        }
        return records;
    }

    /**
     * Every task with live ordinary owned work, mapped to that work: each live non-background task counts for every
     * owner task above it up to and including the first background one. Owner chains must be loaded.
     */
    std::map<std::int64_t, std::vector<std::int64_t>> ownedLive(const OwnershipOverlay* overlay) const {
        std::map<std::int64_t, std::vector<std::int64_t>> owned;
        for (const Json& record : liveRecords(overlay)) {
            if (record.value("background", false)) {
                continue;
            }
            for (const OwnershipStep& step : above(m_records.parentOf(record), overlay)) {
                if (step.kind == "unknown") {
                    break;
                }
                if (step.kind != "task") {
                    continue;
                }
                owned[step.id].push_back(record.at("id").get<std::int64_t>());
                if (step.node.background) {
                    break;
                }
            }
        }
        return owned;
    }

    /**
     * Whether ordinary traversal from `scope` reaches `start`: walking up reaches the scope's conversation, or an
     * ownerless one for `roots`, without crossing a background owner, unless `crossBackground`. Nothing while an edge
     * is not loaded.
     */
    std::optional<bool> inScope(const OwnershipRef& start, const IdleScope& scope, bool crossBackground = false) const {
        for (const OwnershipStep& step : above(start, nullptr)) {
            if (step.kind == "unknown") {
                return std::nullopt;
            }
            if (step.kind == "conversation") {
                if (!scope.roots && step.id == scope.conversation) {
                    return true;
                }
            } else if (step.node.background && !crossBackground) {
                return false;
            }
        }
        return scope.roots;
    }

    /**
     * Whether a live owner's cancellation intent reaches `start`: walking up finds an owner with intent before a
     * background owner without it. Terminal owners never cascade.
     */
    bool belowCancelled(const OwnershipRef& start) const {
        for (const OwnershipStep& step : above(start, nullptr)) {
            if (step.kind == "unknown") {
                return false;
            }
            if (step.kind != "task") {
                continue;
            }
            auto live = m_live.find(step.id);
            if (live != m_live.end() && m_records.cancellationIntent(live->second)) {
                return true;
            }
            if (step.node.background) {
                return false;
            }
        }
        return false;
    }

    /** No live non-background task in the scope; a task whose owner edges are not loaded yet counts as inside. */
    bool idle(const std::optional<std::int64_t>& conversationId) const {
        const IdleScope scope = conversationId ? IdleScope{false, *conversationId} : IdleScope{true, 0};
        for (const auto& item : m_live) {
            if (!item.second.value("background", false) &&
                inScope(m_records.parentOf(item.second), scope) != std::optional<bool>(false)) {
                return false;
            }
        }
        return true;
    }

    OwnershipOverlay overlayOf(Transaction& tx) const {
        OwnershipOverlay overlay;
        for (const Json& record : tx.stagedTasks()) {
            overlay.tasks[record.at("id").get<std::int64_t>()] = record;
        }
        for (const Json& record : tx.stagedConversations()) {
            overlay.edges[record.at("id").get<std::int64_t>()] =
                record.contains("owner") ? std::optional<std::int64_t>(record.at("owner").at("taskId").get<std::int64_t>())
                                         : std::nullopt;
        }
        return overlay;
    }

    // ─── Reservation ────────────────────────────────────────────────────────

    /** Reserves every eligible task in one commit; orphans abort-marked tasks no definition can take. */
    Result<std::vector<TaskReservation>> reserve() {
        std::vector<TaskReservation> reservations;
        auto committed = m_session.commit([&](Transaction& tx) { return reserveInTransaction(tx, reservations); });
        if (!committed) {
            for (const TaskReservation& reservation : reservations) {
                dropInvocation(reservation.invocation);
                reservation.invocation->done.open();
            }
            return std::unexpected(committed.error());
        }
        return reservations;
    }

    Result<void> reserveInTransaction(Transaction& tx, std::vector<TaskReservation>& reservations) {
        if (!enabled() || closing()) {
            return {};
        }
        if (auto loaded = loadScopes(false); !loaded) {
            return std::unexpected(loaded.error());
        }
        const auto owned = ownedLive(nullptr);
        std::shared_ptr<const IRegistrySnapshot> snapshot;
        std::vector<Json> records;
        for (const auto& item : m_live) {
            records.push_back(item.second);
        }
        for (const Json& record : records) {
            const std::int64_t id = record.at("id").get<std::int64_t>();
            if (invocationOf(id) || !waitingOn(record, owned).empty() || record.at("state").at("status") == "completing") {
                continue;
            }
            const std::string mode = record.value("abortRequested", false) ? "abort" : "run";
            if (!snapshot) {
                snapshot = m_registry.snapshot();
            }
            TaskResolution resolution = resolve(record, snapshot);
            if (!resolution.ready) {
                if (mode == "abort") {
                    auto orphaned = terminate(tx, record, Json::object({{"status", "orphaned"}, {"reason", resolution.reason}}));
                    if (!orphaned) {
                        return orphaned;
                    }
                }
                continue;
            }
            if (resolution.migrates || record.at("state").at("status") != "running") {
                Json running = Json::object({{"status", "running"}, {"checkpoint", resolution.record.at("state").at("checkpoint")}});
                if (auto set = tx.setTask(m_records.withState(resolution.record, running)); !set) {
                    return set;
                }
            }
            // Registered on the line, so marks and later reservations see it and close joins it.
            reservations.push_back(TaskReservation{createInvocation(record, mode), resolution.definition, snapshot});
        }
        return {};
    }

    /**
     * Live tasks a task waits for before its next invocation: its live ordinary owned work when abort-marked, since
     * abort runs bottom-up, otherwise the live part of the `on` of a wait.
     */
    std::vector<std::int64_t> waitingOn(const Json& record, const std::map<std::int64_t, std::vector<std::int64_t>>& owned) const {
        if (record.value("abortRequested", false)) {
            auto found = owned.find(record.at("id").get<std::int64_t>());
            return found == owned.end() ? std::vector<std::int64_t>() : found->second;
        }
        std::vector<std::int64_t> waiting;
        if (record.at("state").at("status") != "waiting") {
            return waiting;
        }
        for (const Json& item : record.at("state").at("on")) {
            if (m_live.contains(item.get<std::int64_t>())) {
                waiting.push_back(item.get<std::int64_t>());
            }
        }
        return waiting;
    }

    /** Resolves the record's definition by kind, migrating an older stored version. */
    TaskResolution resolve(const Json& record, const std::shared_ptr<const IRegistrySnapshot>& snapshot) {
        TaskResolution fit = fitOf(record, snapshot->task(record.at("kind").get<std::string>()));
        if (!fit.ready) {
            return fit;
        }
        fit.record = record;
        if (!fit.migrates) {
            return fit;
        }
        const std::int64_t id = record.at("id").get<std::int64_t>();
        const TaskDefinition& definition = *fit.definition;
        const std::string missing = "Task " + record.at("kind").get<std::string>() + " version " +
                                    std::to_string(definition.version) + " has no migration from " +
                                    std::to_string(record.at("version").get<std::int64_t>());
        Result<Json> migrated = definition.migrate
                                    ? definition.migrate(record.at("input"), record.at("state").at("checkpoint"),
                                                         record.at("version").get<std::int64_t>())
                                    : Result<Json>(std::unexpected(Error{"migration_failed", missing}));
        if (!migrated) {
            m_failedMigrations[id] = std::make_pair(fit.definition.get(), migrated.error().message);
            m_callbacks.report(migrated.error());
            return TaskResolution{false, fit.definition, record, false, "migration_failed", migrated.error().message};
        }
        Json next = record;
        next["version"] = definition.version;
        next["input"] = migrated->at("input");
        next["state"]["checkpoint"] = migrated->at("checkpoint");
        fit.record = next;
        return fit;
    }

    TaskResolution fitOf(const Json& record, const std::shared_ptr<const TaskDefinition>& task) const {
        TaskResolution blocked;
        if (!task) {
            blocked.reason = "missing_task";
            return blocked;
        }
        const std::int64_t version = record.at("version").get<std::int64_t>();
        TaskResolution fit;
        fit.ready = true;
        fit.definition = task;
        if (task->version == version) {
            return fit;
        }
        if (task->version < version) {
            blocked.reason = "task_too_old";
            return blocked;
        }
        auto failed = m_failedMigrations.find(record.at("id").get<std::int64_t>());
        if (failed != m_failedMigrations.end() && failed->second.first == task.get()) {
            blocked.reason = "migration_failed";
            blocked.error = failed->second.second;
            return blocked;
        }
        fit.migrates = true;
        return fit;
    }

    TaskInspection inspectTask(const Json& record, const std::shared_ptr<const IRegistrySnapshot>& snapshot,
                               const std::map<std::int64_t, std::vector<std::int64_t>>& owned) {
        TaskInspection inspection;
        inspection.record = record;
        if (invocationOf(record.at("id").get<std::int64_t>())) {
            inspection.state = "running";
            return inspection;
        }
        if (record.at("state").at("status") == "completing") {
            inspection.state = "completing";
            return inspection;
        }
        inspection.on = waitingOn(record, owned);
        if (!inspection.on.empty()) {
            inspection.state = "waiting";
            return inspection;
        }
        TaskResolution fit = fitOf(record, snapshot->task(record.at("kind").get<std::string>()));
        if (!fit.ready) {
            inspection.state = "blocked";
            inspection.reason = fit.reason;
            inspection.error = fit.error;
            return inspection;
        }
        if (fit.migrates && !fit.definition->migrate) {
            inspection.state = "blocked";
            inspection.reason = "migration_failed";
            inspection.error = "Task " + record.at("kind").get<std::string>() + " has no migration";
            return inspection;
        }
        inspection.state = "ready";
        inspection.migrates = fit.migrates;
        return inspection;
    }

    // ─── Invocations ────────────────────────────────────────────────────────

    std::shared_ptr<TaskInvocation> createInvocation(const Json& record, const std::string& mode) {
        auto invocation = std::make_shared<TaskInvocation>();
        invocation->taskId = record.at("id").get<std::int64_t>();
        invocation->conversationId = record.at("conversationId").get<std::int64_t>();
        invocation->mode = mode;
        const std::lock_guard<std::mutex> lock(m_invocationMutex);
        m_invocations[invocation->taskId] = invocation;
        return invocation;
    }

    void dropInvocation(const std::shared_ptr<TaskInvocation>& invocation) {
        const std::lock_guard<std::mutex> lock(m_invocationMutex);
        auto found = m_invocations.find(invocation->taskId);
        if (found != m_invocations.end() && found->second == invocation) {
            m_invocations.erase(found);
        }
    }

    /** Ends an invocation: its runtime operations fail from now on, its signal aborts, and its task is free. */
    void end(const std::shared_ptr<TaskInvocation>& invocation) {
        if (invocation->ended.exchange(true)) {
            return;
        }
        dropInvocation(invocation);
        invocation->signal.abort();
    }

    void start(TaskReservation reservation) {
        const std::lock_guard<std::mutex> lock(m_threadMutex);
        m_threads.emplace_back([this, reservation = std::move(reservation)] {
            if (reservation.invocation->mode == "run") {
                runInvocation(reservation);
            } else {
                runAbort(reservation);
            }
            end(reservation.invocation);
            reservation.invocation->done.open();
            kick();
        });
    }

    /** Runs phase handlers, each preceded by a step that decides on the line whether the invocation continues. */
    void runInvocation(const TaskReservation& reservation) {
        const std::shared_ptr<TaskInvocation> invocation = reservation.invocation;
        std::shared_ptr<const TaskDefinition> definition = reservation.definition;
        std::shared_ptr<const IRegistrySnapshot> snapshot = reservation.snapshot;
        const TaskDefinition* reported = nullptr;
        bool reportedSet = false;
        InvocationRuntime runtime(*this, invocation, definition->name, [&] { return snapshot; });
        std::optional<Json> previousCheckpoint;
        std::optional<std::string> previousFailure;
        while (true) {
            auto current = step(invocation, [&](Transaction& tx, const Json& task) {
                return decide(tx, task, previousCheckpoint, previousFailure, definition, snapshot, reported, reportedSet);
            });
            // Close may seal between the decision and dispatch.
            if (!current || closing()) {
                return;
            }
            const Json checkpoint = current->at("state").at("checkpoint");
            const std::string phase = checkpoint.at("phase").get<std::string>();
            runtime.resetPhase();
            runtime.renameTask(definition->name);
            previousCheckpoint = checkpoint;
            previousFailure.reset();
            auto handler = definition->phases.find(phase);
            if (handler == definition->phases.end()) {
                previousFailure = "Task " + definition->name + " has no phase " + phase;
                continue;
            }
            if (auto ran = handler->second(*current, runtime); !ran) {
                previousFailure = ran.error().message;
            }
        }
    }

    /**
     * Precedence rules for a run invocation, on the line. Terminal, `completing` and `waiting` tasks and a closing
     * harness are applied by `step`. Decides whether the invocation continues with the next phase.
     */
    StepDecision decide(Transaction& tx, const Json& current, const std::optional<Json>& previousCheckpoint,
                        const std::optional<std::string>& previousFailure,
                        std::shared_ptr<const TaskDefinition>& definition,
                        std::shared_ptr<const IRegistrySnapshot>& snapshot, const TaskDefinition*& reported,
                        bool& reportedSet) {
        // 3. abort mark: end; a fresh abort invocation starts once the task's ordinary owned work is gone.
        if (current.value("abortRequested", false)) {
            return StepDecision{"end", ""};
        }
        if (!previousCheckpoint) {
            return StepDecision{"continue", ""};
        }
        // 4. uncaught error.
        if (previousFailure) {
            return StepDecision{"fault", *previousFailure};
        }
        // 6. no durable progress.
        if (m_equality.equal(current.at("state").at("checkpoint"), *previousCheckpoint)) {
            return StepDecision{"fault", "Task " + current.at("kind").get<std::string>() + " phase " +
                                             previousCheckpoint->at("phase").get<std::string>() +
                                             " returned without durable progress"};
        }
        // 5. progress: refresh the snapshot; hand over to a replacement definition that can take the task.
        snapshot = m_registry.snapshot();
        std::shared_ptr<const TaskDefinition> next = snapshot->task(current.at("kind").get<std::string>());
        if (next != definition) {
            if (next && m_records.canReserve(*next, current)) {
                Json pending = Json::object({{"status", "pending"}, {"checkpoint", current.at("state").at("checkpoint")}});
                if (auto set = tx.setTask(m_records.withState(current, pending)); !set) {
                    return StepDecision{"fault", set.error().message};
                }
                return StepDecision{"end", ""};
            }
            if (!reportedSet || reported != next.get()) {
                reportedSet = true;
                reported = next.get();
                m_callbacks.report(Error{next ? "incompatible_task" : "missing_task",
                                         "Task " + std::to_string(current.at("id").get<std::int64_t>()) +
                                             " keeps running under its old " + current.at("kind").get<std::string>() +
                                             " definition"});
            }
        }
        return StepDecision{"continue", ""};
    }

    /** Runs the abort handler once; returning without an outcome faults. */
    void runAbort(const TaskReservation& reservation) {
        const std::shared_ptr<TaskInvocation> invocation = reservation.invocation;
        std::optional<Json> current;
        auto read = m_session.readOnLine([&]() -> Result<void> {
            auto found = m_live.find(invocation->taskId);
            if (found != m_live.end() && found->second.at("state").at("status") == "running") {
                current = found->second;
            }
            return {};
        });
        if (!read || !current || closing()) {
            return;
        }
        std::optional<std::string> failure;
        std::shared_ptr<const IRegistrySnapshot> snapshot = reservation.snapshot;
        InvocationRuntime runtime(*this, invocation, reservation.definition->name, [&] { return snapshot; });
        if (reservation.definition->abort) {
            if (auto ran = reservation.definition->abort(*current, runtime); !ran) {
                failure = ran.error().message;
            }
        }
        const std::string message =
            failure ? *failure
                    : "Abort handler of task " + std::to_string(invocation->taskId) + " returned without a terminal outcome";
        step(invocation, [&](Transaction&, const Json&) { return StepDecision{"fault", message}; });
    }

    /**
     * One synchronous decision on the session line. A task that is no longer running (terminal, `completing` or
     * `waiting`) or a closing harness ends the invocation without a write; otherwise `decide` may stage a write and
     * says whether the invocation continues. Ending happens inside the callback, before a fault's harness cleanup. A
     * failed step, such as admission after close, also ends the invocation.
     */
    std::optional<Json> step(const std::shared_ptr<TaskInvocation>& invocation,
                             const std::function<StepDecision(Transaction&, const Json&)>& decide) {
        std::optional<Json> result;
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto found = m_live.find(invocation->taskId);
            const bool running = found != m_live.end() && found->second.at("state").at("status") == "running";
            const std::optional<Json> current = running ? std::optional<Json>(found->second) : std::nullopt;
            const StepDecision decision = current && !closing() ? decide(tx, *current) : StepDecision{"end", ""};
            if (decision.kind == "continue") {
                result = current;
                return {};
            }
            end(invocation);
            if (decision.kind == "fault") {
                return terminate(tx, *current, Json::object({{"status", "faulted"}, {"error", Json::object({{"message", decision.message}})}}));
            }
            return {};
        });
        if (!committed) {
            end(invocation);
            if (!closing()) {
                m_callbacks.report(committed.error());
            }
            return std::nullopt;
        }
        return result;
    }

    /**
     * Writes an outcome the scheduler decided. While the task's ordinary owned work is live it holds as `completing`
     * and its harness cleanup waits for the final commit; otherwise it is terminal with its cleanup.
     */
    Result<void> terminate(Transaction& tx, const Json& record, const Json& outcome) {
        if (auto loaded = loadScopes(false); !loaded) {
            return std::unexpected(loaded.error());
        }
        const OwnershipOverlay overlay = overlayOf(tx);
        if (ownedLive(&overlay).contains(record.at("id").get<std::int64_t>())) {
            return tx.setTask(m_records.withState(record, Json::object({{"status", "completing"}, {"outcome", outcome}})));
        }
        if (auto set = tx.setTask(m_records.withState(record, Json::object({{"status", "terminal"}, {"outcome", outcome}}))); !set) {
            return set;
        }
        return m_callbacks.settleOutcome(tx, record, outcome);
    }

    /**
     * Replaces a running task's state with what it committed. A terminal state holds as `completing` while ordinary
     * owned work is live, judged on the commit's candidates, so work the same commit creates below the task counts. A
     * wait is validated first.
     */
    Result<void> commitState(Transaction& tx, const TaskInvocation& invocation, const Json& current, const Json& next) {
        const std::string status = next.at("status").get<std::string>();
        if (status == "waiting") {
            if (auto valid = validateWait(tx, invocation, current, next.at("on"), next.at("policy").get<std::string>()); !valid) {
                return valid;
            }
        }
        if (status == "terminal") {
            const OwnershipOverlay overlay = overlayOf(tx);
            if (auto loaded = loadScopes(false); !loaded) {
                return std::unexpected(loaded.error());
            }
            for (const auto& item : overlay.tasks) {
                if (auto chain = loadChain(m_records.parentOf(item.second), &overlay); !chain) {
                    return chain;
                }
            }
            if (ownedLive(&overlay).contains(current.at("id").get<std::int64_t>())) {
                return tx.setTask(m_records.withState(current, Json::object({{"status", "completing"}, {"outcome", next.at("outcome")}})));
            }
        }
        return tx.setTask(m_records.withState(current, next));
    }

    /**
     * A wait names existing tasks other than the waiter and its owners, which could never finish first; `failFast`
     * only tasks the waiter owns. An abort handler cannot wait.
     */
    Result<void> validateWait(Transaction& tx, const TaskInvocation& invocation, const Json& current, const Json& on,
                              const std::string& policy) {
        const std::int64_t id = current.at("id").get<std::int64_t>();
        if (invocation.mode == "abort") {
            return std::unexpected(Error{"durable_error", "Abort handler of task " + std::to_string(id) + " cannot wait"});
        }
        const OwnershipOverlay overlay = overlayOf(tx);
        if (auto chain = loadChain(m_records.parentOf(current), nullptr); !chain) {
            return chain;
        }
        std::set<std::int64_t> owners;
        for (const OwnershipStep& step : above(m_records.parentOf(current), nullptr)) {
            if (step.kind == "task") {
                owners.insert(step.id);
            }
        }
        for (const Json& item : on) {
            const std::int64_t waitedId = item.get<std::int64_t>();
            if (waitedId == id || owners.contains(waitedId)) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(id) + " cannot wait on itself or its owner " + std::to_string(waitedId)});
            }
            std::optional<Json> member;
            if (auto staged = overlay.tasks.find(waitedId); staged != overlay.tasks.end()) {
                member = staged->second;
            } else if (auto live = m_live.find(waitedId); live != m_live.end()) {
                member = live->second;
            } else {
                auto stored = storage().task(waitedId);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                member = *stored;
            }
            if (!member) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(waitedId) + " does not exist"});
            }
            if (policy == "failFast" && !(member->contains("owner") && member->at("owner").get<std::int64_t>() == id)) {
                return std::unexpected(Error{"durable_error", "Task " + std::to_string(id) + " can wait failFast only on tasks it owns; " + std::to_string(waitedId) + " is not one"});
            }
        }
        return {};
    }

    /** Commits after rereading the task on the line and gating the invocation. */
    Result<void> gated(TaskInvocation& invocation, const std::function<Result<void>(Transaction&, const Json&)>& change) {
        if (auto live = checkLive(invocation); !live) {
            return std::unexpected(live.error());
        }
        const auto committed = m_session.commit(
            [&](Transaction& tx) -> Result<void> {
                if (invocation.ended) {
                    return std::unexpected(endedError(invocation));
                }
                if (closing()) {
                    return std::unexpected(closedError());
                }
                auto found = m_live.find(invocation.taskId);
                const std::string label = "Task " + std::to_string(invocation.taskId);
                if (found == m_live.end()) {
                    return std::unexpected(Error{"durable_error", label + " is terminal"});
                }
                if (found->second.at("state").at("status") != "running") {
                    return std::unexpected(Error{"durable_error", label + " is " + found->second.at("state").at("status").get<std::string>()});
                }
                if (invocation.mode == "run" && found->second.value("abortRequested", false)) {
                    return std::unexpected(Error{"durable_error", label + " has a durable abort mark"});
                }
                return change(tx, found->second);
            },
            TransactionScope{invocation.conversationId, invocation.taskId});
        return committed ? Result<void>() : std::unexpected(committed.error());
    }

    DurableSession& m_session;
    IRegistryReader& m_registry;
    SchedulerCallbacks m_callbacks;
    TaskRecords m_records;
    JsonEquality m_equality;

    // Protected by the session line.
    std::map<std::int64_t, Json> m_live;
    std::map<std::int64_t, std::pair<const TaskDefinition*, std::string>> m_failedMigrations;
    std::map<std::int64_t, std::optional<std::int64_t>> m_edges;
    std::set<std::int64_t> m_conversationOwners;
    std::map<std::int64_t, TaskNode> m_settled;
    JsonWaiters m_taskWaiters;
    JsonWaiters m_idleWaiters;

    // Wake state.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_enabled = false;
    bool m_closing = false;
    bool m_dirty = false;
    bool m_reconcileRequested = false;
    bool m_cascadePending = false;
    bool m_stop = false;
    std::set<std::int64_t> m_failFastChecks;
    std::int64_t m_registrySubscription = 0;
    std::int64_t m_lineSubscription = 0;
    std::int64_t m_closeSubscription = 0;

    std::mutex m_invocationMutex;
    std::map<std::int64_t, std::shared_ptr<TaskInvocation>> m_invocations;
    std::mutex m_threadMutex;
    std::vector<std::thread> m_threads;
    std::thread m_worker;

    static constexpr std::size_t kMaxDepth = 100000;
};
