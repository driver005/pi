#include <gtest/gtest.h>

import std;
import pi.durable.memory_storage;
import pi.support.durable_session;
import pi.support.tool_call_api;

/** An ITaskRuntime whose commits run on a real session, enough for a tool call's slot publishing. */
class SessionRuntime : public ITaskRuntime {
public:
    explicit SessionRuntime(DurableSession& session, std::int64_t conversationId)
        : m_session(session), m_conversationId(conversationId) {}

    std::int64_t taskId() const override {
        return 7;
    }
    std::int64_t conversationId() const override {
        return m_conversationId;
    }
    AbortSignal& signal() override {
        return m_signal;
    }
    Result<std::shared_ptr<const IConversationAgent>> agent() override {
        return std::shared_ptr<const IConversationAgent>();
    }
    IModelRuntime* models() override {
        return nullptr;
    }
    ResolvedSettings settings() override {
        return ResolvedSettings{};
    }
    Result<std::shared_ptr<IExecutionEnv>> env() override {
        return std::shared_ptr<IExecutionEnv>();
    }
    Result<void> eachHook(const std::string&, const std::function<Result<void>(const HookHandler&)>&) override {
        return {};
    }
    Result<void> commit(const std::function<Result<std::optional<Json>>(Transaction&, const Json&)>& change) override {
        auto committed = m_session.commit([&](Transaction& tx) -> Result<void> {
            auto next = change(tx, Json::object({{"id", 7}}));
            return next ? Result<void>() : std::unexpected(next.error());
        });
        return committed ? Result<void>() : std::unexpected(committed.error());
    }
    Result<std::optional<Json>> memo(const std::string&) override {
        return std::optional<Json>();
    }
    Result<Json> memo(const std::string&, const Json& candidate) override {
        return candidate;
    }
    Result<std::optional<Json>> snapshot(const DocDefinition&, const DocAddressArgs&) override {
        return std::optional<Json>();
    }
    Result<std::optional<Json>> snapshotAsOf(const DocDefinition&, const DocAddressArgs&, std::int64_t) override {
        return std::optional<Json>();
    }
    Result<Json> newTask(const std::string& name, const Json&) override {
        if (name != "known") {
            return std::unexpected(Error{"missing_task", "Task " + name + " is not registered"});
        }
        return Json::object({{"version", 1}, {"checkpoint", Json::object({{"phase", "start"}})}});
    }
    Result<std::optional<Json>> getTask(std::int64_t) override {
        return std::optional<Json>();
    }
    Result<Json> waitForTask(std::int64_t, const AbortSignal*) override {
        return Json::object();
    }
    Result<std::vector<Json>> outcomes(const std::vector<std::int64_t>&) override {
        return std::vector<Json>();
    }
    Result<std::shared_ptr<IConversationHandle>> conversation(std::int64_t) override {
        return std::shared_ptr<IConversationHandle>();
    }
    Result<std::optional<Json>> entry(std::int64_t, const std::optional<std::string>&) override {
        return std::optional<Json>();
    }
    Result<Json> context(std::int64_t, const std::optional<std::int64_t>&) override {
        return Json::object();
    }
    std::int64_t now() override {
        return 1;
    }
    void report(const Error& error) override {
        m_reports.push_back(error.message);
    }
    Result<void> sleep(std::int64_t, const AbortSignal*) override {
        return {};
    }

    std::vector<std::string> m_reports;

private:
    DurableSession& m_session;
    std::int64_t m_conversationId;
    AbortSignal m_signal;
};

class ToolCallApiTest : public ::testing::Test {
protected:
    ToolCallApiTest() : m_session(m_storage) {
        EXPECT_TRUE(m_session.commit([&](Transaction& tx) -> Result<void> {
            auto created = tx.createConversation(Json::object({{"kind", "ownerless"}}));
            if (!created) {
                return std::unexpected(created.error());
            }
            m_conversation = created->at("id").get<std::int64_t>();
            DocAddressArgs args;
            args.owner = m_conversation;
            auto live = tx.doc(m_documents.live(), args);
            if (!live) {
                return std::unexpected(live.error());
            }
            (**live)["tools"] = Json::array({Json::object({{"callId", "c1"}, {"name", "t"}, {"taskId", 7}, {"status", "running"}})});
            return {};
        }).has_value());
        m_runtime = std::make_unique<SessionRuntime>(m_session, m_conversation);
    }

    Json slot() {
        DocAddressArgs args;
        args.owner = m_conversation;
        auto live = m_session.snapshot(m_documents.live(), args);
        return live && *live ? (*live)->at("tools")[0] : Json(nullptr);
    }

    template <typename Predicate>
    bool eventually(Predicate predicate) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::shared_ptr<MemoryStorage> m_storage = std::make_shared<MemoryStorage>();
    DurableSession m_session;
    BuiltinDocuments m_documents;
    std::int64_t m_conversation = 0;
    std::unique_ptr<SessionRuntime> m_runtime;
};

TEST_F(ToolCallApiTest, OutputIsPublishedIntoTheSlotAndRetainedForTheResult) {
    ToolCallApi api(*m_runtime, "c1", OutputLimits{}, nullptr);
    ASSERT_TRUE(api.output("hello ").has_value());
    ASSERT_TRUE(api.output("world\n").has_value());
    ASSERT_TRUE(eventually([&] { return slot().value("output", std::string()) == "hello world\n"; }));
    EXPECT_EQ(api.retainedOutput().text, "hello world\n");
    auto waiters = api.finish();
    EXPECT_TRUE(waiters.empty());
    auto after = api.output("late");
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, "call_settled");
}

TEST_F(ToolCallApiTest, DetailsWaitForTheirCommitAndDiagnosticsAreAppendedOnce) {
    ToolCallApi api(*m_runtime, "c1", OutputLimits{}, nullptr);
    ASSERT_TRUE(api.details(Json::object({{"progress", 0.5}})).has_value());
    EXPECT_EQ(slot().at("details").at("progress"), 0.5);
    ToolDiagnostic note;
    note.severity = "info";
    note.message = "spilled";
    note.code = "spill";
    ASSERT_TRUE(api.diagnostic(note).has_value());
    ASSERT_TRUE(api.details(Json::object({{"progress", 1.0}})).has_value());
    EXPECT_EQ(slot().at("details").at("progress"), 1.0);
    ASSERT_EQ(slot().at("diagnostics").size(), 1u);
    EXPECT_EQ(slot().at("diagnostics")[0].at("code"), "spill");
    ASSERT_TRUE(api.details(Json::object({{"progress", 1.0}, {"done", true}})).has_value());
    EXPECT_EQ(slot().at("diagnostics").size(), 1u);
    EXPECT_EQ(api.reportedDiagnostics().size(), 1u);
    EXPECT_EQ(api.reportedDetails()->at("done"), true);
    (void)api.finish();
}

TEST_F(ToolCallApiTest, DroppedOutputIsRecordedOnTheSlot) {
    OutputLimits limits;
    limits.maxLines = 1;
    limits.retain = "tail";
    ToolCallApi api(*m_runtime, "c1", limits, nullptr);
    ASSERT_TRUE(api.output("a\nb\nc\n").has_value());
    ASSERT_TRUE(eventually([&] { return slot().value("output", std::string()) == "c\n"; }));
    EXPECT_EQ(slot().at("droppedLines"), 2);
    EXPECT_EQ(slot().at("droppedBytes"), 4);
    (void)api.finish();
}

TEST_F(ToolCallApiTest, CommitAndCreateTaskGoThroughTheRuntime) {
    ToolCallApi api(*m_runtime, "c1", OutputLimits{}, nullptr);
    bool ran = false;
    ASSERT_TRUE(api.commit([&](Transaction&) -> Result<void> {
        ran = true;
        return {};
    }).has_value());
    EXPECT_TRUE(ran);
    auto failed = api.commit([](Transaction&) -> Result<void> { return std::unexpected(Error{"x", "change failed"}); });
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "change failed");
    TaskOptions options;
    options.ownership = Json::object({{"kind", "conversation"}});
    options.conversationId = m_conversation;
    auto created = api.createTask("known", Json::object(), options);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    auto stored = m_storage->task(*created);
    ASSERT_TRUE(stored->has_value());
    EXPECT_EQ((*stored)->at("kind"), "known");
    EXPECT_FALSE(api.createTask("unknown", Json::object(), options).has_value());
    EXPECT_EQ(api.taskId(), 7);
    EXPECT_EQ(api.callId(), "c1");
    (void)api.finish();
}
