#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.support.compaction_task_definition;
import pi.support.context_reader;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.task_fixture;

class CompactionTaskTest : public ::testing::Test {
protected:
    CompactionTaskTest() : m_fixture({CompactionTaskDefinition().build()}), m_faux(m_executor, m_clock) {
        Model model;
        model.id = "m";
        model.provider = "faux";
        model.api = "faux";
        model.contextWindow = 100000;
        model.maxTokens = 4096;
        m_models.addModel(model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_requests.push_back(context);
                m_maxTokens.push_back(options.maxTokens);
            }
            return m_faux.stream(model, context, options);
        });
        m_fixture.setModels(&m_models);
        m_fixture.setContextReader([this](std::int64_t conversationId, const std::optional<std::int64_t>& at) {
            return m_reader.read(m_fixture.session(), conversationId, at);
        });
        m_fixture.setAgentFactory([this](std::int64_t) { return agent(m_withModel); });
        ResolvedSettings settings;
        settings.compaction.keepRecentTokens = 30;
        settings.retry.baseDelayMs = 100;
        m_fixture.setSettings(settings);
    }

    void SetUp() override {
        m_fixture.open();
    }

    std::shared_ptr<const IAgent> agent(bool withModel) {
        auto snapshot = std::make_shared<AgentSnapshot>();
        if (withModel) {
            snapshot->model = Json::object({{"provider", "faux"}, {"modelId", "m"}});
        }
        auto extension = std::make_shared<Extension>();
        extension->name = "hooks";
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            extension->hooks = m_hooks;
        }
        return std::make_shared<ResolvedAgent>(snapshot, std::vector<PromptSection>{}, std::vector<std::shared_ptr<const Extension>>{extension});
    }

    void addHook(HookHandler handler) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        HookRegistration registration;
        registration.task = "pi.compaction";
        registration.handlers["beforeCompact"] = std::move(handler);
        m_hooks.push_back(std::move(registration));
    }

    Json message(const std::string& role, const std::string& text) {
        if (role == "user") {
            return Json::object({{"role", "user"}, {"content", text}, {"timestamp", 1}});
        }
        return Json::object({{"role", "assistant"}, {"content", Json::array({Json::object({{"type", "text"}, {"text", text}})})},
                             {"stopReason", "stop"}, {"timestamp", 2},
                             {"usage", Json::object({{"input", 1}, {"output", 1}, {"cacheRead", 0}, {"cacheWrite", 0}, {"totalTokens", 2},
                                                     {"cost", Json::object({{"input", 0.0}, {"output", 0.0}, {"cacheRead", 0.0}, {"cacheWrite", 0.0}, {"total", 0.0}})}})}});
    }

    /** Six entries of 100 characters each: user, assistant, user, assistant, user, assistant. */
    std::int64_t conversationWithHistory() {
        const std::int64_t conversationId = m_fixture.newConversation();
        for (int i = 0; i < 6; ++i) {
            const std::string text = std::string(i + 1, 'a') + std::string(100 - (i + 1), '.');
            m_fixture.append(conversationId, Json::object({{"kind", "m"}, {"model", Json::array({message(i % 2 == 0 ? "user" : "assistant", text)})}}));
        }
        return conversationId;
    }

    /** Creates the compaction task with its live status, as the harness does. */
    std::int64_t compact(std::int64_t conversationId, const std::string& reason = "manual", const std::optional<std::string>& instructions = std::nullopt,
                         const std::optional<std::int64_t>& owner = std::nullopt) {
        Json input = Json::object({{"reason", reason}});
        if (instructions) {
            input["instructions"] = *instructions;
        }
        std::int64_t id = 0;
        (void)m_fixture.session().commit([&](Transaction& tx) -> Result<void> {
            auto created = m_planner.createCompaction(tx, conversationId, input, owner);
            if (!created) {
                return std::unexpected(created.error());
            }
            id = *created;
            return {};
        });
        return id;
    }

    void run(std::int64_t taskId) {
        m_fixture.resume();
        ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(taskId); })) << m_fixture.status(taskId);
    }

    std::string phase(std::int64_t taskId) {
        auto record = m_fixture.task(taskId);
        return record ? record->at("state").value("checkpoint", Json::object()).value("phase", std::string()) : std::string();
    }

    Json outcome(std::int64_t taskId) {
        return m_fixture.task(taskId)->at("state").at("outcome");
    }

    Json live(std::int64_t conversationId) {
        return m_fixture.document(m_documents.live(), conversationId);
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    TaskFixture m_fixture;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    ContextReader m_reader;
    CompactionPlanner m_planner;
    BuiltinDocuments m_documents;
    std::mutex m_mutex;
    std::vector<HookRegistration> m_hooks;
    std::vector<TranscriptContext> m_requests;
    std::vector<std::optional<std::int64_t>> m_maxTokens;
    bool m_withModel = true;
};

TEST_F(CompactionTaskTest, ConversationOwnedCompactionPlacesItsSummaryThroughAWriteSubmission) {
    m_faux.enqueue(m_faux.textResponse("## Goal\nsummary body"));
    const std::int64_t conversationId = conversationWithHistory();
    const std::int64_t id = compact(conversationId, "manual", std::string("focus on files"));
    run(id);
    ASSERT_EQ(outcome(id).at("status"), "completed");
    const Json result = outcome(id).at("result");
    ASSERT_TRUE(result.contains("submissionId"));
    EXPECT_EQ(m_fixture.submission(result.at("submissionId").get<std::int64_t>())->at("status"), "done");
    // The summary is the newest head marker; the context is the summary followed by the two kept entries.
    auto view = m_reader.read(m_fixture.session(), conversationId, std::nullopt);
    ASSERT_TRUE(view.has_value());
    ASSERT_EQ(view->at("messages").size(), 3u);
    const std::string summaryText = view->at("messages")[0].at("content")[0].at("text").get<std::string>();
    EXPECT_EQ(summaryText.rfind("The conversation history before this point was compacted into the following summary:\n\n<summary>\n## Goal\nsummary body\n</summary>", 0), 0u);
    EXPECT_EQ(view->at("head").at("model")[0].at("role"), "user");
    EXPECT_EQ(view->at("head").at("data").at("reason"), "manual");
    EXPECT_EQ(view->at("head").at("head"), view->at("entries")[1].at("id"));
    // The summarizer saw a system prompt and the serialized older entries plus the extra focus.
    ASSERT_EQ(m_requests.size(), 1u);
    ASSERT_EQ(m_requests[0].messages.size(), 2u);
    EXPECT_EQ(m_maxTokens[0], std::optional<std::int64_t>(4096));
    // The status is gone and the model's usage is in the ledger.
    EXPECT_FALSE(live(conversationId).contains("compactions"));
    EXPECT_TRUE(m_fixture.document(m_documents.usage(), conversationId).at("models").contains("faux/m"));
}

TEST_F(CompactionTaskTest, BlockingCompactionAppendsItsSummaryDirectly) {
    m_faux.enqueue(m_faux.textResponse("blocking summary"));
    const std::int64_t conversationId = conversationWithHistory();
    // The owning task stands in for a generation: it just waits for the compaction.
    auto owner = std::make_shared<TaskDefinition>();
    owner->name = "owner";
    owner->initial = [](const Json&) { return Json::object({{"phase", "wait"}}); };
    owner->phases["wait"] = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        (void)WaitGate().wait({&runtime.signal()});
        return {};
    };
    owner->abort = [](const Json&, ITaskRuntime& runtime) -> Result<void> {
        return runtime.commit([](Transaction&, const Json&) -> Result<std::optional<Json>> {
            return std::optional<Json>(Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "aborted"}})}}));
        });
    };
    m_fixture.install("owner", {owner});
    const std::int64_t ownerId = m_fixture.createTask(conversationId, "owner", Json::object());
    const std::int64_t id = compact(conversationId, "threshold", std::nullopt, ownerId);
    m_fixture.resume();
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(id); }));
    ASSERT_EQ(outcome(id).at("status"), "completed");
    const Json result = outcome(id).at("result");
    ASSERT_TRUE(result.contains("entryId"));
    auto view = m_reader.read(m_fixture.session(), conversationId, std::nullopt);
    EXPECT_EQ(view->at("head").at("id"), result.at("entryId"));
    EXPECT_EQ(view->at("head").at("data").at("reason"), "threshold");
    ASSERT_TRUE(m_fixture.scheduler().abort(ownerId).has_value());
}

TEST_F(CompactionTaskTest, NothingToCompactCompletesWithAnEmptyResult) {
    const std::int64_t conversationId = m_fixture.newConversation();
    m_fixture.append(conversationId, Json::object({{"kind", "m"}, {"model", Json::array({message("user", "short")})}}));
    const std::int64_t id = compact(conversationId);
    run(id);
    EXPECT_EQ(outcome(id).at("status"), "completed");
    EXPECT_TRUE(outcome(id).at("result").empty());
    EXPECT_TRUE(m_requests.empty());
    EXPECT_FALSE(live(conversationId).contains("compactions"));
}

TEST_F(CompactionTaskTest, HooksCanDeclineOrSupplyTheSummary) {
    const std::int64_t conversationId = conversationWithHistory();
    std::atomic<bool> decline{true};
    addHook([&](const Json& compaction, IHookApi&) -> Result<std::optional<Json>> {
        EXPECT_EQ(compaction.at("reason"), "manual");
        EXPECT_EQ(compaction.at("entries").size(), 4u);
        EXPECT_EQ(compaction.at("firstKept"), compaction.at("entries")[0].at("id").get<std::int64_t>() + 4);
        if (decline) {
            return std::optional<Json>(Json::object({{"decline", true}}));
        }
        return std::optional<Json>(Json::object({{"summary", "hook summary"}}));
    });
    const std::int64_t declined = compact(conversationId);
    run(declined);
    EXPECT_TRUE(outcome(declined).at("result").empty());
    EXPECT_TRUE(m_requests.empty());
    decline = false;
    const std::int64_t supplied = compact(conversationId);
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(supplied); }));
    EXPECT_TRUE(outcome(supplied).at("result").contains("submissionId"));
    EXPECT_TRUE(m_requests.empty());
    auto view = m_reader.read(m_fixture.session(), conversationId, std::nullopt);
    EXPECT_NE(view->at("messages")[0].at("content")[0].at("text").get<std::string>().find("hook summary"), std::string::npos);
}

TEST_F(CompactionTaskTest, RetryableFailuresBackOffDurablyThenSucceed) {
    AssistantMessage failure;
    failure.stopReason = StopReason::Error;
    failure.errorMessage = "503 service unavailable";
    m_faux.enqueue(failure);
    m_faux.enqueue(m_faux.textResponse("second try"));
    const std::int64_t conversationId = conversationWithHistory();
    const std::int64_t id = compact(conversationId);
    m_fixture.resume();
    ASSERT_TRUE(m_fixture.eventually([&] { return phase(id) == "retry" || m_fixture.terminal(id); }));
    ASSERT_EQ(phase(id), "retry") << m_fixture.task(id)->dump();
    EXPECT_TRUE(live(conversationId).at("compactions")[0].contains("retry"));
    EXPECT_EQ(live(conversationId).at("compactions")[0].at("retry").at("error"), "503 service unavailable");
    // The backoff waits for the harness clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(m_fixture.status(id), "running");
    m_fixture.clock() += 1000;
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(id); }));
    EXPECT_EQ(outcome(id).at("status"), "completed");
    EXPECT_EQ(m_faux.callCount(), 2);
    EXPECT_FALSE(live(conversationId).contains("compactions"));
}

TEST_F(CompactionTaskTest, NonRetryableFailureEndsFailedAndClearsTheStatus) {
    AssistantMessage failure;
    failure.stopReason = StopReason::Error;
    failure.errorMessage = "invalid request";
    m_faux.enqueue(failure);
    const std::int64_t conversationId = conversationWithHistory();
    const std::int64_t id = compact(conversationId);
    run(id);
    EXPECT_EQ(outcome(id).at("status"), "failed");
    EXPECT_EQ(outcome(id).at("error").at("message"), "Summarization failed: invalid request");
    EXPECT_EQ(outcome(id).at("error").at("detail").at("reason"), "model_error");
    EXPECT_FALSE(live(conversationId).contains("compactions"));
}

TEST_F(CompactionTaskTest, MissingModelFailsWithNoModel) {
    m_withModel = false;
    const std::int64_t conversationId = conversationWithHistory();
    const std::int64_t id = compact(conversationId);
    run(id);
    EXPECT_EQ(outcome(id).at("status"), "failed");
    EXPECT_EQ(outcome(id).at("error").at("message"), "No model is configured");
    EXPECT_EQ(outcome(id).at("error").at("detail").at("reason"), "no_model");
}

TEST_F(CompactionTaskTest, AbortRemovesTheStatusAndEndsAborted) {
    WaitGate summarizing;
    WaitGate release;
    m_models.setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        summarizing.open();
        (void)release.wait({options.signal.get()});
        return m_faux.stream(model, context, options);
    });
    m_faux.enqueue(m_faux.textResponse("late"));
    const std::int64_t conversationId = conversationWithHistory();
    // The status a creator would have registered with the task.
    const std::int64_t id = compact(conversationId);
    m_fixture.resume();
    ASSERT_TRUE(summarizing.wait({}).has_value());
    std::thread aborter([&] { (void)m_fixture.scheduler().abort(id); });
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.task(id)->value("abortRequested", false); }));
    release.open();
    aborter.join();
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(id); }));
    EXPECT_EQ(outcome(id).at("status"), "aborted");
    EXPECT_TRUE(m_fixture.entries(conversationId).size() == 6u);
}
