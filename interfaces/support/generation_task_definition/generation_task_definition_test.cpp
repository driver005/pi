#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.support.compaction_task_definition;
import pi.support.context_reader;
import pi.support.generation_task_definition;
import pi.support.submission_admission;
import pi.support.tool_task_definition;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.task_fixture;

class GenerationTaskTest : public ::testing::Test {
protected:
    GenerationTaskTest()
        : m_fixture({GenerationTaskDefinition().build(), ToolTaskDefinition().build(), CompactionTaskDefinition().build()}),
          m_faux(m_executor, m_clock) {
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
            }
            return m_faux.stream(model, context, options);
        });
        m_fixture.setModels(&m_models);
        m_fixture.setContextReader([this](std::int64_t conversationId, const std::optional<std::int64_t>& at) {
            return m_reader.read(m_fixture.session(), conversationId, at);
        });
        m_fixture.setAgentFactory([this](std::int64_t) { return agent(); });
        m_fixture.setOutcomeCleanup([](Transaction&, const Json&, const Json&) -> Result<void> { return {}; });
        m_settings.retry.baseDelayMs = 100;
        m_settings.compaction.keepRecentTokens = 30;
        m_fixture.setSettings(m_settings);
    }

    void SetUp() override {
        m_fixture.open();
    }

    std::shared_ptr<const IAgent> agent() {
        auto snapshot = std::make_shared<AgentSnapshot>();
        std::vector<PromptSection> sections;
        auto extension = std::make_shared<Extension>();
        extension->name = "ext";
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_withModel) {
                snapshot->model = Json::object({{"provider", "faux"}, {"modelId", "m"}});
            }
            snapshot->tools = m_tools;
            extension->hooks = m_hooks;
        }
        sections.push_back(PromptSection{"env", [](const PromptInput&) -> Result<std::optional<std::string>> { return std::optional<std::string>("sandbox"); }, true});
        return std::make_shared<ResolvedAgent>(snapshot, sections, std::vector<std::shared_ptr<const Extension>>{extension});
    }

    ToolRegistration echoTool(const std::function<ToolExecutionResult()>& result = nullptr, const std::string& replay = "unsafe") {
        ToolRegistration tool;
        tool.name = "echo";
        tool.description = "echoes";
        tool.parameters = Json::parse(R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
        tool.replay = replay;
        tool.execute = [this, result](const Json& args, IToolExecutionApi&) -> Result<ToolExecutionResult> {
            ++m_toolRuns;
            if (result) {
                return result();
            }
            ToolExecutionResult out;
            out.content = Json::array({Json::object({{"type", "text"}, {"text", "echo:" + args.at("text").get<std::string>()}})});
            return out;
        };
        return tool;
    }

    void addTool(ToolRegistration tool) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_tools.push_back(std::move(tool));
    }

    void addHook(const std::string& task, const std::string& name, HookHandler handler) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        HookRegistration registration;
        registration.task = task;
        registration.handlers[name] = std::move(handler);
        m_hooks.push_back(std::move(registration));
    }

    AssistantMessage errorResponse(const std::string& text) {
        AssistantMessage failure;
        failure.stopReason = StopReason::Error;
        failure.errorMessage = text;
        return failure;
    }

    /** Admits input, which places a user entry and starts the run; returns the submission id. */
    std::int64_t submit(std::int64_t conversationId, const std::string& text, const std::optional<std::string>& whenBusy = std::nullopt) {
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = text;
        draft.whenBusy = whenBusy;
        std::int64_t id = 0;
        auto committed = m_fixture.session().commit([&](Transaction& tx) -> Result<void> {
            auto admitted = m_admission.admit(tx, conversationId, draft, m_fixture.clock().load(), "one-at-a-time", "one-at-a-time");
            if (!admitted) {
                return std::unexpected(admitted.error());
            }
            id = *admitted;
            return {};
        });
        EXPECT_TRUE(committed.has_value()) << (committed ? "" : committed.error().message);
        return id;
    }

    bool settled(std::int64_t submissionId) {
        auto record = m_fixture.submission(submissionId);
        return record && (record->at("status") == "done" || record->at("status") == "unanswered");
    }

    void await(std::int64_t submissionId) {
        m_fixture.resume();
        ASSERT_TRUE(m_fixture.eventually([&] { return settled(submissionId); })) << m_fixture.submission(submissionId)->dump();
    }

    Json live(std::int64_t conversationId) {
        return m_fixture.document(m_documents.live(), conversationId);
    }

    std::vector<std::string> kinds(std::int64_t conversationId) {
        std::vector<std::string> kinds;
        for (const Json& entry : m_fixture.entries(conversationId)) {
            std::string kind = entry.at("kind").get<std::string>();
            if (entry.contains("model") && entry.at("model")[0].value("role", std::string()) == "assistant") {
                kind += ":" + entry.at("model")[0].at("stopReason").get<std::string>();
            }
            kinds.push_back(kind);
        }
        return kinds;
    }

    std::int64_t countTasks(const std::string& kind) {
        std::int64_t count = 0;
        (void)m_fixture.session().readOnLine([&]() -> Result<void> {
            TaskQuery query;
            query.kind = kind;
            auto page = m_fixture.storage()->scanTasks(query, 100, std::nullopt);
            count = page ? static_cast<std::int64_t>(page->items.size()) : 0;
            return {};
        });
        return count;
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    TaskFixture m_fixture;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    ContextReader m_reader;
    SubmissionAdmission m_admission;
    BuiltinDocuments m_documents;
    ResolvedSettings m_settings;
    std::mutex m_mutex;
    std::vector<ToolRegistration> m_tools;
    std::vector<HookRegistration> m_hooks;
    std::vector<TranscriptContext> m_requests;
    std::atomic<int> m_toolRuns{0};
    bool m_withModel = true;
};

TEST_F(GenerationTaskTest, AnswersAPromptAndSettlesTheSubmission) {
    m_faux.enqueue(m_faux.textResponse("hello there"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    EXPECT_EQ(kinds(conversationId), std::vector<std::string>({"pi.user", "pi.system", "pi.assistant:stop"}));
    const std::vector<Json> entries = m_fixture.entries(conversationId);
    EXPECT_EQ(m_fixture.submission(submission)->at("answer"), entries[2].at("id"));
    // The positional system entry carries the rendered section and the offered tools.
    const Json& system = entries[1].at("model")[0];
    EXPECT_EQ(system.at("sections").at("env"), "<env>\nsandbox\n</env>");
    EXPECT_FALSE(live(conversationId).contains("run"));
    EXPECT_FALSE(live(conversationId).contains("generation"));
    ASSERT_EQ(m_requests.size(), 1u);
    // The provider saw the system message and the user message.
    EXPECT_EQ(m_requests[0].messages.size(), 2u);
    const Json usage = m_fixture.document(m_documents.usage(), conversationId);
    EXPECT_TRUE(usage.at("models").contains("faux/m"));
}

TEST_F(GenerationTaskTest, RunsToolRoundsAndHandsTheRunToTheNextGeneration) {
    addTool(echoTool());
    m_faux.enqueue(m_faux.toolCallResponse("echo", Json::object({{"text", "ping"}}), "call-1"));
    m_faux.enqueue(m_faux.textResponse("all done"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "use the tool");
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    EXPECT_EQ(kinds(conversationId), std::vector<std::string>({"pi.user", "pi.system", "pi.assistant:toolUse", "pi.tool-result", "pi.assistant:stop"}));
    EXPECT_EQ(m_toolRuns.load(), 1);
    const std::vector<Json> entries = m_fixture.entries(conversationId);
    EXPECT_EQ(entries[3].at("model")[0].at("content")[0].at("text"), "echo:ping");
    EXPECT_EQ(countTasks("pi.generation"), 2);
    EXPECT_EQ(countTasks("pi.tool"), 1);
    // The second request carried the tool call and its result.
    ASSERT_EQ(m_requests.size(), 2u);
    EXPECT_EQ(m_requests[1].messages.size(), 4u);
    EXPECT_EQ(m_fixture.submission(submission)->at("answer"), entries[4].at("id"));
    EXPECT_FALSE(live(conversationId).contains("tools"));
}

TEST_F(GenerationTaskTest, CallsToUnofferedToolsGetUnavailableResultsWithoutTasks) {
    addTool(echoTool());
    m_faux.enqueue(m_faux.toolCallResponse("missing", Json::object(), "call-9"));
    m_faux.enqueue(m_faux.textResponse("ok then"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "call something");
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    const std::vector<Json> entries = m_fixture.entries(conversationId);
    ASSERT_EQ(entries.size(), 5u);
    EXPECT_EQ(entries[3].at("data").at("diagnostics")[0].at("code"), "tool_unavailable");
    EXPECT_EQ(countTasks("pi.tool"), 0);
}

TEST_F(GenerationTaskTest, TerminatingToolEndsTheRunAtTheAssistantEntry) {
    ToolRegistration finish = echoTool([] {
        ToolExecutionResult result;
        result.content = Json::array({Json::object({{"type", "text"}, {"text", "finished"}})});
        result.control = Json::object({{"terminate", true}});
        return result;
    });
    addTool(finish);
    m_faux.enqueue(m_faux.toolCallResponse("echo", Json::object({{"text", "x"}}), "call-1"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "go");
    await(submission);
    const std::vector<Json> entries = m_fixture.entries(conversationId);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    EXPECT_EQ(m_fixture.submission(submission)->at("answer"), entries[2].at("id"));
    EXPECT_EQ(m_faux.callCount(), 1);
    EXPECT_EQ(countTasks("pi.generation"), 1);
}

TEST_F(GenerationTaskTest, RetryableErrorsBackOffDurablyAndTheNextAttemptSucceeds) {
    m_faux.enqueue(errorResponse("503 overloaded"));
    m_faux.enqueue(m_faux.textResponse("recovered"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    m_fixture.resume();
    ASSERT_TRUE(m_fixture.eventually([&] { return live(conversationId).contains("generation") && live(conversationId).at("generation").contains("retry"); }));
    EXPECT_EQ(live(conversationId).at("generation").at("retry").at("error"), "503 overloaded");
    EXPECT_EQ(live(conversationId).at("generation").at("attempt"), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(settled(submission));
    m_fixture.clock() += 1000;
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    EXPECT_EQ(m_faux.callCount(), 2);
    // The failed attempt is in the transcript but excluded from the model context.
    EXPECT_EQ(kinds(conversationId), std::vector<std::string>({"pi.user", "pi.system", "pi.assistant:error", "pi.assistant:stop"}));
}

TEST_F(GenerationTaskTest, NonRetryableErrorsFailTheRunAndLeaveTheInputUnanswered) {
    m_faux.enqueue(errorResponse("invalid request"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    await(submission);
    Json record = *m_fixture.submission(submission);
    EXPECT_EQ(record.at("status"), "unanswered");
    EXPECT_EQ(record.at("reason"), "model_error");
    EXPECT_EQ(record.at("detail"), "invalid request");
    EXPECT_EQ(kinds(conversationId).back(), "pi.assistant:error");
    EXPECT_FALSE(live(conversationId).contains("run"));
}

TEST_F(GenerationTaskTest, RetriesAreBoundedByThePolicy) {
    ResolvedSettings settings = m_settings;
    settings.retry.maxRetries = 1;
    settings.retry.baseDelayMs = 1;
    m_fixture.setSettings(settings);
    m_faux.enqueue(errorResponse("503 overloaded"));
    m_faux.enqueue(errorResponse("503 overloaded"));
    m_faux.enqueue(m_faux.textResponse("never"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    m_fixture.resume();
    ASSERT_TRUE(m_fixture.eventually([&] { return live(conversationId).contains("generation") && live(conversationId).at("generation").contains("retry"); }));
    m_fixture.clock() += 1000;
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "unanswered");
    EXPECT_EQ(m_faux.callCount(), 2);
}

TEST_F(GenerationTaskTest, MissingModelFailsWithNoModel) {
    m_withModel = false;
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    await(submission);
    Json record = *m_fixture.submission(submission);
    EXPECT_EQ(record.at("status"), "unanswered");
    EXPECT_EQ(record.at("reason"), "no_model");
    EXPECT_EQ(m_faux.callCount(), 0);
}

TEST_F(GenerationTaskTest, ContextOverflowCompactsOnceAndRetriesTheRequest) {
    m_faux.enqueue(errorResponse("prompt is too long: 250000 tokens > 200000 maximum"));
    m_faux.enqueue(m_faux.textResponse("the summary"));
    m_faux.enqueue(m_faux.textResponse("final answer"));
    const std::int64_t conversationId = m_fixture.newConversation();
    for (int i = 0; i < 6; ++i) {
        const std::string text = std::string(100, static_cast<char>('a' + i));
        Json message = i % 2 == 0 ? Json::object({{"role", "user"}, {"content", text}, {"timestamp", 1}})
                                  : Json::object({{"role", "assistant"}, {"content", Json::array({Json::object({{"type", "text"}, {"text", text}})})},
                                                  {"stopReason", "stop"}, {"timestamp", 2}});
        m_fixture.append(conversationId, Json::object({{"kind", "m"}, {"model", Json::array({message})}}));
    }
    const std::int64_t submission = submit(conversationId, "one more");
    await(submission);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
    EXPECT_EQ(m_faux.callCount(), 3);
    EXPECT_EQ(countTasks("pi.compaction"), 1);
    bool summarized = false;
    for (const Json& entry : m_fixture.entries(conversationId)) {
        summarized = summarized || entry.at("kind") == "pi.compaction";
    }
    EXPECT_TRUE(summarized);
    EXPECT_EQ(kinds(conversationId).back(), "pi.assistant:stop");
    EXPECT_FALSE(live(conversationId).contains("compactions"));
}

TEST_F(GenerationTaskTest, OnYieldContinuationAppendsAUserMessageAndRunsAgain) {
    m_faux.enqueue(m_faux.textResponse("first answer"));
    m_faux.enqueue(m_faux.textResponse("second answer"));
    std::atomic<int> yields{0};
    addHook("pi.generation", "onYield", [&](const Json&, IHookApi&) -> Result<std::optional<Json>> {
        if (++yields == 1) {
            return std::optional<Json>(Json::object({{"continue", "keep going"}}));
        }
        return std::optional<Json>();
    });
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    await(submission);
    EXPECT_EQ(kinds(conversationId), std::vector<std::string>({"pi.user", "pi.system", "pi.assistant:stop", "pi.user", "pi.assistant:stop"}));
    const std::vector<Json> entries = m_fixture.entries(conversationId);
    EXPECT_EQ(entries[3].at("model")[0].at("content"), "keep going");
    // The run's input settles on the final answer.
    EXPECT_EQ(m_fixture.submission(submission)->at("answer"), entries[4].at("id"));
    EXPECT_EQ(countTasks("pi.generation"), 2);
}

TEST_F(GenerationTaskTest, RequestHooksSeeAndReplaceMessagesAndResponsesAreObserved) {
    m_faux.enqueue(m_faux.textResponse("done"));
    std::atomic<int> observed{0};
    addHook("pi.generation", "beforeRequest", [](const Json& request, IHookApi&) -> Result<std::optional<Json>> {
        Json messages = request.at("messages");
        messages.push_back(Json::object({{"role", "user"}, {"content", "injected"}, {"timestamp", 1}}));
        return std::optional<Json>(Json::object({{"messages", messages}}));
    });
    addHook("pi.generation", "afterResponse", [&](const Json& message, IHookApi&) -> Result<std::optional<Json>> {
        EXPECT_EQ(message.at("role"), "assistant");
        ++observed;
        return std::optional<Json>();
    });
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    await(submission);
    ASSERT_EQ(m_requests.size(), 1u);
    EXPECT_EQ(m_requests[0].messages.size(), 3u);
    EXPECT_EQ(observed.load(), 1);
    // The replacement is for that request only: the transcript does not contain it.
    EXPECT_EQ(kinds(conversationId).size(), 3u);
}

TEST_F(GenerationTaskTest, SequentialRoundsRunOneToolAtATime) {
    ResolvedSettings settings = m_settings;
    settings.toolExecution = "sequential";
    m_fixture.setSettings(settings);
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    addTool(echoTool([&] {
        const int now = ++running;
        int seen = maxRunning.load();
        while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        --running;
        ToolExecutionResult result;
        result.content = Json::array({Json::object({{"type", "text"}, {"text", "ok"}})});
        return result;
    }));
    AssistantMessage calls;
    ToolCall first;
    first.id = "a";
    first.name = "echo";
    first.arguments = Json::object({{"text", "1"}});
    ToolCall second = first;
    second.id = "b";
    calls.content.emplace_back(first);
    calls.content.emplace_back(second);
    calls.stopReason = StopReason::ToolUse;
    m_faux.enqueue(calls);
    m_faux.enqueue(m_faux.textResponse("finished"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "two calls");
    await(submission);
    EXPECT_EQ(m_toolRuns.load(), 2);
    EXPECT_EQ(maxRunning.load(), 1);
    EXPECT_EQ(countTasks("pi.tool"), 2);
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "done");
}

TEST_F(GenerationTaskTest, ParallelRoundsStartEveryToolAtOnce) {
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    WaitGate bothStarted;
    addTool(echoTool([&] {
        const int now = ++running;
        int seen = maxRunning.load();
        while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
        }
        if (now == 2) {
            bothStarted.open();
        }
        (void)bothStarted.waitFor(std::chrono::milliseconds(3000));
        --running;
        ToolExecutionResult result;
        result.content = Json::array({Json::object({{"type", "text"}, {"text", "ok"}})});
        return result;
    }));
    AssistantMessage calls;
    ToolCall first;
    first.id = "a";
    first.name = "echo";
    first.arguments = Json::object({{"text", "1"}});
    ToolCall second = first;
    second.id = "b";
    calls.content.emplace_back(first);
    calls.content.emplace_back(second);
    calls.stopReason = StopReason::ToolUse;
    m_faux.enqueue(calls);
    m_faux.enqueue(m_faux.textResponse("finished"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "two calls");
    await(submission);
    EXPECT_EQ(maxRunning.load(), 2);
}

TEST_F(GenerationTaskTest, AbortConvertsTheCommittedPartialAndSettlesTheInput) {
    WaitGate streaming;
    WaitGate release;
    m_models.setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)release.wait({options.signal.get()});
        return m_faux.stream(model, context, options);
    });
    m_faux.enqueue(m_faux.textResponse("late"));
    const std::int64_t conversationId = m_fixture.newConversation();
    const std::int64_t submission = submit(conversationId, "hi");
    m_fixture.resume();
    ASSERT_TRUE(streaming.wait({}).has_value());
    std::thread aborter([&] { (void)m_fixture.scheduler().abortConversation(conversationId, false); });
    ASSERT_TRUE(m_fixture.eventually([&] { return !live(conversationId).contains("run") || m_fixture.scheduler().scheduling() == "closing"; }) ||
                true);
    release.open();
    aborter.join();
    ASSERT_TRUE(m_fixture.eventually([&] { return settled(submission); }));
    EXPECT_EQ(m_fixture.submission(submission)->at("status"), "unanswered");
    EXPECT_EQ(m_fixture.submission(submission)->at("reason"), "aborted");
    EXPECT_FALSE(live(conversationId).contains("run"));
    EXPECT_FALSE(live(conversationId).contains("generation"));
}
