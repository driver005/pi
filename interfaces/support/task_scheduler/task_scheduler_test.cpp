#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.registry;
import pi.support.task_scheduler;

class EmptyAgent : public IAgent {
public:
    std::shared_ptr<const AgentSnapshot> snapshot() const override {
        return std::make_shared<AgentSnapshot>();
    }

    const std::vector<PromptSection>& sections() const override {
        return m_sections;
    }

    std::vector<HookRegistration> hooks(const std::string&) const override {
        return {};
    }

private:
    std::vector<PromptSection> m_sections;
};

class SchedulerTest : public ::testing::Test {
protected:
    ~SchedulerTest() override {
        closeSession();
    }

    // ─── Fixture plumbing ───────────────────────────────────────────────────

    void openScheduler() {
        m_session = std::make_unique<DurableSession>(m_storage, nullptr, [this] {
            if (m_scheduler) {
                m_scheduler->join();
            }
        });
        SchedulerCallbacks callbacks;
        callbacks.agent = [](std::int64_t, const std::shared_ptr<const IRegistrySnapshot>&) -> Result<std::shared_ptr<const IAgent>> {
            return std::shared_ptr<const IAgent>(std::make_shared<EmptyAgent>());
        };
        callbacks.settings = [] { return ResolvedSettings{}; };
        callbacks.env = [](std::int64_t) -> Result<std::shared_ptr<IExecutionEnv>> { return std::shared_ptr<IExecutionEnv>(); };
        callbacks.now = [this] { return m_clock.load(); };
        callbacks.report = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_reportMutex);
            m_reports.push_back(error.message);
        };
        callbacks.settleOutcome = [this](Transaction&, const Json& record, const Json& outcome) -> Result<void> {
            const std::lock_guard<std::mutex> lock(m_reportMutex);
            m_settled.push_back(record.at("id").get<std::int64_t>());
            m_settledOutcomes.push_back(outcome);
            return {};
        };
        callbacks.withdrawInputs = [](Transaction&, std::int64_t) -> Result<void> { return {}; };
        callbacks.conversation = [](std::int64_t, const std::shared_ptr<TaskInvocation>&) -> Result<std::shared_ptr<IConversationHandle>> {
            return std::shared_ptr<IConversationHandle>();
        };
        callbacks.context = [](std::int64_t, const std::optional<std::int64_t>&) -> Result<Json> { return Json::object(); };
        m_scheduler = std::make_unique<TaskScheduler>(*m_session, m_registry, std::move(callbacks));
        ASSERT_TRUE(m_scheduler->open().has_value());
    }

    void closeSession() {
        if (m_session) {
            (void)m_session->close();
        }
        m_scheduler.reset();
        m_session.reset();
    }

    template <typename Predicate>
    bool eventually(Predicate predicate) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::int64_t newConversation(const Json& ownership = Json::object({{"kind", "ownerless"}})) {
        std::int64_t id = 0;
        auto committed = m_session->commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(ownership);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = created->at("id").get<std::int64_t>();
            return {};
        });
        EXPECT_TRUE(committed.has_value());
        return id;
    }

    std::int64_t createTask(std::int64_t conversationId, const std::string& name, const Json& input = Json::object(),
                            std::int64_t version = 1, const Json& ownership = Json::object({{"kind", "conversation"}}),
                            bool background = false) {
        TaskOptions options;
        options.ownership = ownership;
        options.conversationId = conversationId;
        options.background = background;
        std::int64_t id = 0;
        auto committed = m_session->commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createTask(name, version, input, Json::object({{"phase", "start"}}), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return {};
        });
        EXPECT_TRUE(committed.has_value()) << (committed ? "" : committed.error().message);
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

    std::string outcomeStatus(std::int64_t id) {
        auto record = task(id);
        if (!record || record->at("state").at("status") != "terminal") {
            return "";
        }
        return record->at("state").at("outcome").at("status").get<std::string>();
    }

    bool terminal(std::int64_t id) {
        return status(id) == "terminal";
    }

    void install(const std::string& extensionName, std::vector<std::shared_ptr<const TaskDefinition>> tasks) {
        Extension extension;
        extension.name = extensionName;
        extension.tasks = std::move(tasks);
        ASSERT_TRUE(m_registry.install(std::move(extension)).has_value());
    }

    Json completed(const Json& result) {
        return Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "completed"}, {"result", result}})}});
    }

    Json aborted() {
        return Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "aborted"}})}});
    }

    Json running(const std::string& phase) {
        return Json::object({{"status", "running"}, {"checkpoint", Json::object({{"phase", phase}})}});
    }

    std::shared_ptr<TaskDefinition> definition(const std::string& name, std::int64_t version = 1) {
        auto result = std::make_shared<TaskDefinition>();
        result->name = name;
        result->version = version;
        result->initial = [](const Json&) { return Json::object({{"phase", "start"}}); };
        result->abort = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
            return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(aborted()); });
        };
        return result;
    }

    /** A two-phase task: `start` advances the checkpoint, `finish` completes with `result`. */
    std::shared_ptr<TaskDefinition> twoPhase(const std::string& name, const Json& result) {
        auto result_ = definition(name);
        result_->phases["start"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
            return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(running("finish")); });
        };
        result_->phases["finish"] = [this, result](const Json&, ITaskRuntime& runtime) -> Result<void> {
            return runtime.commit([this, result](Transaction&, const Json&) -> Result<std::optional<Json>> {
                return std::optional<Json>(completed(result));
            });
        };
        return result_;
    }

    std::vector<std::string> reports() {
        const std::lock_guard<std::mutex> lock(m_reportMutex);
        return m_reports;
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    Registry m_registry;
    std::unique_ptr<DurableSession> m_session;
    std::unique_ptr<TaskScheduler> m_scheduler;
    std::atomic<std::int64_t> m_clock{1000};
    std::mutex m_reportMutex;
    std::vector<std::string> m_reports;
    std::vector<std::int64_t> m_settled;
    std::vector<Json> m_settledOutcomes;
    std::int64_t m_taskId = 0;
};

TEST_F(SchedulerTest, ContinuesThroughCheckpointProgressAndCompletes) {
    install("tasks", {twoPhase("work", Json::object({{"answer", 42}}))});
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t id = createTask(conversationId, "work");
    m_scheduler->resume();
    auto settled = m_scheduler->waitForTask(id);
    ASSERT_TRUE(settled.has_value()) << settled.error().message;
    EXPECT_EQ(settled->at("state").at("outcome").at("result").at("answer"), 42);
    EXPECT_FALSE(settled->contains("memos"));
}

TEST_F(SchedulerTest, NothingRunsUntilResumed) {
    install("tasks", {twoPhase("work", Json(1))});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "work");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(status(id), "pending");
    EXPECT_EQ(m_scheduler->scheduling(), "paused");
    m_scheduler->resume();
    EXPECT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_EQ(m_scheduler->scheduling(), "running");
}

TEST_F(SchedulerTest, FaultsAPhaseWithoutDurableProgress) {
    auto lazy = definition("lazy");
    lazy->phases["start"] = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(); });
    };
    install("tasks", {lazy});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "lazy");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_EQ(outcomeStatus(id), "faulted");
    EXPECT_NE(task(id)->at("state").at("outcome").at("error").at("message").get<std::string>().find("returned without durable progress"),
              std::string::npos);
    EXPECT_EQ(m_settled, std::vector<std::int64_t>({id}));
}

TEST_F(SchedulerTest, FaultsAFailingPhaseButKeepsACommittedOutcome) {
    auto failing = definition("failing");
    failing->phases["start"] = [](const Json&, ITaskRuntime&) -> Result<void> { return std::unexpected(Error{"boom", "phase threw"}); };
    auto late = definition("late");
    late->phases["start"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        if (auto done = runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json(7))); }); !done) {
            return done;
        }
        return std::unexpected(Error{"boom", "threw afterwards"});
    };
    install("tasks", {failing, late});
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t failId = createTask(conversationId, "failing");
    const std::int64_t lateId = createTask(conversationId, "late");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(failId) && terminal(lateId); }));
    EXPECT_EQ(outcomeStatus(failId), "faulted");
    EXPECT_EQ(task(failId)->at("state").at("outcome").at("error").at("message"), "phase threw");
    EXPECT_EQ(outcomeStatus(lateId), "completed");
}

TEST_F(SchedulerTest, ComparesCheckpointsByValueIgnoringKeyOrder) {
    auto reorder = definition("reorder");
    reorder->phases["start"] = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([](Transaction&, const Json&) -> Result<std::optional<Json>> {
            // The same checkpoint with its keys in another order is no progress.
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", Json::parse(R"({"phase":"start"})")}}));
        });
    };
    install("tasks", {reorder});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "reorder");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_EQ(outcomeStatus(id), "faulted");
}

TEST_F(SchedulerTest, WaitingTaskResumesOnceEveryAwaitedTaskIsTerminal) {
    WaitGate release;
    auto slow = definition("slow");
    slow->phases["start"] = [this, &release](const Json&, ITaskRuntime& runtime) -> Result<void> {
        (void)release.wait({&runtime.signal()});
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json(1))); });
    };
    auto waiter = definition("waiter");
    waiter->phases["start"] = [](const Json& record, ITaskRuntime& runtime) -> Result<void> {
        const Json on = record.at("input").at("on");
        return runtime.commit([on](Transaction&, const Json&) -> Result<std::optional<Json>> {
            return std::optional<Json>(Json::object({{"status", "waiting"},
                                                     {"checkpoint", Json::object({{"phase", "after"}})},
                                                     {"on", on},
                                                     {"policy", "allSettled"}}));
        });
    };
    waiter->phases["after"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json("woke"))); });
    };
    install("tasks", {slow, waiter});
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t slowId = createTask(conversationId, "slow");
    const std::int64_t waiterId = createTask(conversationId, "waiter", Json::object({{"on", Json::array({slowId})}}));
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return status(waiterId) == "waiting"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(status(waiterId), "waiting");
    release.open();
    ASSERT_TRUE(eventually([&] { return terminal(waiterId); }));
    EXPECT_EQ(task(waiterId)->at("state").at("outcome").at("result"), "woke");
}

TEST_F(SchedulerTest, MemosAreFirstWriterWinsAndVanishAtTerminal) {
    std::mutex mutex;
    std::vector<Json> seen;
    auto memo = definition("memo");
    memo->phases["start"] = [&, this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        auto first = runtime.memo("k", Json("one"));
        auto second = runtime.memo("k", Json("two"));
        auto read = runtime.memo("k");
        auto absent = runtime.memo("other");
        if (!first || !second || !read || !absent) {
            return std::unexpected(Error{"memo", "memo failed"});
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            seen = {*first, *second, **read, Json(absent->has_value())};
        }
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json(0))); });
    };
    install("tasks", {memo});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "memo");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], "one");
    EXPECT_EQ(seen[1], "one");
    EXPECT_EQ(seen[2], "one");
    EXPECT_EQ(seen[3], false);
    EXPECT_FALSE(task(id)->contains("memos"));
}

TEST_F(SchedulerTest, AbortSignalsTheRunJoinsItAndRunsTheAbortHandler) {
    WaitGate started;
    std::atomic<bool> sawSignal{false};
    std::atomic<bool> commitRejected{false};
    auto blocker = definition("blocker");
    blocker->phases["start"] = [&, this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        started.open();
        (void)WaitGate().wait({&runtime.signal()});
        sawSignal = true;
        // After the mark the run may not write.
        auto rejected = runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(running("never")); });
        commitRejected = !rejected.has_value();
        return {};
    };
    install("tasks", {blocker});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "blocker");
    m_scheduler->resume();
    ASSERT_TRUE(started.wait({}).has_value());
    auto result = m_scheduler->abort(id);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "marked");
    EXPECT_TRUE(sawSignal);
    EXPECT_TRUE(commitRejected);
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_EQ(outcomeStatus(id), "aborted");
    auto again = m_scheduler->abort(id);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, "terminal");
}

TEST_F(SchedulerTest, AbortOfATaskNoDefinitionCanTakeSettlesItOrphaned) {
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "ghost");
    auto result = m_scheduler->abort(id);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_EQ(outcomeStatus(id), "orphaned");
    EXPECT_EQ(task(id)->at("state").at("outcome").at("reason"), "missing_task");
    EXPECT_EQ(m_settled, std::vector<std::int64_t>({id}));
    EXPECT_FALSE(m_scheduler->abort(999999).has_value());
}

TEST_F(SchedulerTest, WaitForTaskAndIdleCoverBlockedWorkAndIgnoreBackgroundTasks) {
    install("tasks", {twoPhase("work", Json(3))});
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t ghost = createTask(conversationId, "ghost");
    std::atomic<bool> idle{false};
    std::thread waiter([&] {
        EXPECT_TRUE(m_scheduler->waitForIdle(conversationId).has_value());
        idle = true;
    });
    m_scheduler->resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(idle);
    ASSERT_TRUE(m_scheduler->abort(ghost).has_value());
    waiter.join();
    EXPECT_TRUE(idle);
    // A background task does not hold the conversation busy.
    const std::int64_t background = createTask(conversationId, "ghost", Json::object(), 1, Json::object({{"kind", "conversation"}}), true);
    EXPECT_TRUE(m_scheduler->waitForIdle(conversationId).has_value());
    EXPECT_TRUE(m_scheduler->waitForIdle(std::nullopt).has_value());
    ASSERT_TRUE(m_scheduler->abort(background).has_value());
    EXPECT_FALSE(m_scheduler->waitForTask(123456).has_value());
}

TEST_F(SchedulerTest, WaitsCancelledByTheirSignalFailAndCloseRejectsPendingWaits) {
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t ghost = createTask(conversationId, "ghost");
    AbortSignal cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancel.abort();
    });
    auto cancelled = m_scheduler->waitForTask(ghost, &cancel);
    canceller.join();
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code, "aborted");
    std::atomic<bool> rejected{false};
    std::thread pending([&] {
        auto waited = m_scheduler->waitForTask(ghost);
        rejected = !waited.has_value() && waited.error().code == "harness_closed";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    closeSession();
    pending.join();
    EXPECT_TRUE(rejected);
}

TEST_F(SchedulerTest, RecoversRunningTasksAsPendingAtOpenAndRerunsThem) {
    // A first process leaves a running task behind; the second one reopens the same storage.
    {
        install("tasks", {definition("hang")});
        openScheduler();
        const std::int64_t conversationId = newConversation();
        m_taskId = createTask(conversationId, "hang");
        ASSERT_TRUE(m_session->commit([&](Transaction& tx) -> Result<void> {
            auto record = tx.task(m_taskId);
            if (!record || !*record) {
                return std::unexpected(Error{"x", "no task"});
            }
            Json state = Json::object({{"status", "running"}, {"checkpoint", Json::object({{"phase", "start"}})}});
            return tx.setTask(TaskRecords().withState(**record, state));
        }).has_value());
        EXPECT_EQ(status(m_taskId), "running");
        // Dropped without a close: the storage is what survives.
        m_scheduler->join();
        m_scheduler.reset();
        m_session.reset();
    }
    auto reborn = twoPhase("hang", Json("recovered"));
    Extension replacement;
    replacement.name = "tasks";
    replacement.tasks = {reborn};
    ASSERT_TRUE(m_registry.install(std::move(replacement)).has_value());
    openScheduler();
    EXPECT_EQ(status(m_taskId), "pending");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(m_taskId); }));
    EXPECT_EQ(task(m_taskId)->at("state").at("outcome").at("result"), "recovered");
}

TEST_F(SchedulerTest, MigratesAnOlderRecordAtReservationOrBlocksIt) {
    auto v2 = twoPhase("work", Json("v2"));
    v2->version = 2;
    v2->migrate = [](const Json& input, const Json&, std::int64_t from) -> Result<Json> {
        return Json::object({{"input", Json::object({{"from", from}, {"was", input}})}, {"checkpoint", Json::object({{"phase", "finish"}})}});
    };
    install("tasks", {v2});
    openScheduler();
    const std::int64_t conversationId = newConversation();
    const std::int64_t old = createTask(conversationId, "work", Json::object({{"n", 1}}), 1);
    const std::int64_t future = createTask(conversationId, "work", Json::object(), 3);
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(old); }));
    auto record = task(old);
    EXPECT_EQ(record->at("version"), 2);
    EXPECT_EQ(record->at("input").at("from"), 1);
    EXPECT_EQ(status(future), "pending");
    auto inspected = std::vector<TaskInspection>();
    ASSERT_TRUE(m_session->readOnLine([&]() -> Result<void> {
        auto result = m_scheduler->inspect(m_registry.snapshot());
        if (!result) {
            return std::unexpected(result.error());
        }
        inspected = *result;
        return {};
    }).has_value());
    ASSERT_EQ(inspected.size(), 1u);
    EXPECT_EQ(inspected[0].state, "blocked");
    EXPECT_EQ(inspected[0].reason, "task_too_old");
}

TEST_F(SchedulerTest, FailingMigrationBlocksTheTaskAndIsReportedOnce) {
    auto v2 = twoPhase("work", Json(0));
    v2->version = 2;
    v2->migrate = [](const Json&, const Json&, std::int64_t) -> Result<Json> { return std::unexpected(Error{"bad", "cannot migrate"}); };
    install("tasks", {v2});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "work", Json::object(), 1);
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return !reports().empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(status(id), "pending");
    EXPECT_EQ(reports().size(), 1u);
    EXPECT_EQ(reports()[0], "cannot migrate");
}

TEST_F(SchedulerTest, OwnerHoldsItsOutcomeUntilItsChildFinishes) {
    WaitGate releaseChild;
    auto child = definition("child");
    child->phases["start"] = [this, &releaseChild](const Json&, ITaskRuntime& runtime) -> Result<void> {
        (void)releaseChild.wait({&runtime.signal()});
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json("child"))); });
    };
    auto parent = definition("parent");
    parent->phases["start"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([this](Transaction& tx, const Json& current) -> Result<std::optional<Json>> {
            TaskOptions options;
            options.ownership = Json::object({{"kind", "task"}, {"taskId", current.at("id")}});
            auto created = tx.createTask("child", 1, Json::object(), Json::object({{"phase", "start"}}), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            return std::optional<Json>(running("finish"));
        });
    };
    parent->phases["finish"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        // The parent completes while its child is live; the outcome is held until the child ends.
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json("parent"))); });
    };
    install("tasks", {parent, child});
    openScheduler();
    const std::int64_t parentId = createTask(newConversation(), "parent");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return status(parentId) == "completing"; })) << status(parentId) << " reports " << (reports().empty() ? "" : reports()[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(status(parentId), "completing");
    releaseChild.open();
    ASSERT_TRUE(eventually([&] { return terminal(parentId); }));
    EXPECT_EQ(task(parentId)->at("state").at("outcome").at("result"), "parent");
}

TEST_F(SchedulerTest, FailedOwnerCancelsItsChildren) {
    auto child = definition("child");
    child->phases["start"] = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        (void)WaitGate().wait({&runtime.signal()});
        return {};
    };
    auto parent = definition("parent");
    parent->phases["start"] = [this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([this](Transaction& tx, const Json& current) -> Result<std::optional<Json>> {
            TaskOptions options;
            options.ownership = Json::object({{"kind", "task"}, {"taskId", current.at("id")}});
            auto created = tx.createTask("child", 1, Json::object(), Json::object({{"phase", "start"}}), options);
            if (!created) {
                return std::unexpected(created.error());
            }
            return std::optional<Json>(running("finish"));
        });
    };
    parent->phases["finish"] = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([](Transaction&, const Json&) -> Result<std::optional<Json>> {
            return std::optional<Json>(Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "failed"}, {"error", Json::object({{"message", "nope"}})}})}}));
        });
    };
    install("tasks", {parent, child});
    openScheduler();
    const std::int64_t parentId = createTask(newConversation(), "parent");
    m_scheduler->resume();
    ASSERT_TRUE(eventually([&] { return terminal(parentId); }));
    EXPECT_EQ(outcomeStatus(parentId), "failed");
    // The child was aborted by the cascade and settled through its abort handler.
    std::int64_t childId = 0;
    ASSERT_TRUE(m_session->readOnLine([&]() -> Result<void> {
        TaskQuery query;
        query.kind = "child";
        auto page = m_storage->scanTasks(query, 10, std::nullopt);
        if (page && !page->items.empty()) {
            childId = page->items[0].at("id").get<std::int64_t>();
        }
        return {};
    }).has_value());
    ASSERT_NE(childId, 0);
    EXPECT_TRUE(eventually([&] { return outcomeStatus(childId) == "aborted"; }));
}

TEST_F(SchedulerTest, SleepWaitsForTheClockAndAbortInterruptsIt) {
    std::atomic<bool> slept{false};
    WaitGate started;
    auto sleeper = definition("sleeper");
    sleeper->phases["start"] = [&, this](const Json&, ITaskRuntime& runtime) -> Result<void> {
        started.open();
        if (auto woke = runtime.sleep(m_clock.load() + 500); !woke) {
            return std::unexpected(woke.error());
        }
        slept = true;
        return runtime.commit([this](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(completed(Json(1))); });
    };
    install("tasks", {sleeper});
    openScheduler();
    const std::int64_t id = createTask(newConversation(), "sleeper");
    m_scheduler->resume();
    ASSERT_TRUE(started.wait({}).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_FALSE(slept);
    m_clock += 600;
    ASSERT_TRUE(eventually([&] { return terminal(id); }));
    EXPECT_TRUE(slept);
}

TEST_F(SchedulerTest, CloseSignalsHandlersJoinsThemAndWritesNoOutcome) {
    WaitGate started;
    std::atomic<bool> signalled{false};
    auto blocker = definition("blocker");
    blocker->phases["start"] = [&](const Json&, ITaskRuntime& runtime) -> Result<void> {
        started.open();
        (void)WaitGate().wait({&runtime.signal()});
        signalled = runtime.signal().aborted();
        return {};
    };
    install("tasks", {blocker});
    openScheduler();
    (void)createTask(newConversation(), "blocker");
    m_scheduler->resume();
    ASSERT_TRUE(started.wait({}).has_value());
    closeSession();
    EXPECT_TRUE(signalled);
    // The handler returned without progress after the seal, but no fault was written for it.
    EXPECT_TRUE(m_settled.empty());
}
