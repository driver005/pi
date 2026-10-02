#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.session.agent_session;
import pi.testing.fake_model_runtime;
import pi.testing.fake_resource_loader;
import pi.testing.fake_settings_manager;
import pi.testing.scripted_tool;
import pi.testing.session_harness;
import pi.tools.tool_registry;

class AgentSessionTest : public testing::Test {
protected:
    AgentSessionTest() : m_harness("/tmp") {
        m_model.id = "faux-1";
        m_model.provider = "faux";
        m_model.api = "faux";
        m_model.contextWindow = 100000;
        m_model.maxTokens = 8000;
        m_models.addModel(m_model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_harness.provider().stream(model, context, options);
        });
        m_readTool = std::make_shared<ScriptedTool>("read", "contents");
        m_readTool->setPrompt("Read file contents", {"Use read to examine files."});
        m_tools.add(m_readTool);
        rebuild();
    }

    void rebuild() {
        AgentSessionConfig config{m_harness.agents(), m_harness.session(), m_settings, m_models, m_resources,
                                  m_tools, m_bash, m_harness.files(), m_harness.clock(), m_harness.ids(),
                                  m_harness.sleeper(), m_model, ThinkingLevel::Off, "/tmp", std::nullopt,
                                  std::nullopt, {}, {}};
        m_session = std::make_unique<AgentSession>(config);
        m_session->subscribe([this](const AgentSessionEvent& event) { m_events.push_back(event); });
    }

    std::vector<AgentSessionEvent> eventsOf(SessionEventType type) const {
        std::vector<AgentSessionEvent> out;
        for (const auto& event : m_events) {
            if (event.type == type) {
                out.push_back(event);
            }
        }
        return out;
    }

    std::vector<std::string> entryTypes() {
        std::vector<std::string> types;
        for (const auto& entry : m_harness.session().entries()) {
            types.push_back(entry.type);
        }
        return types;
    }

    AssistantMessage failure(const std::string& error) {
        AssistantMessage message = m_harness.provider().textResponse("partial");
        message.stopReason = StopReason::Error;
        message.errorMessage = error;
        return message;
    }

    void seedHistory(int turns) {
        for (int i = 0; i < turns; ++i) {
            UserMessage user;
            user.content = std::string(1200, 'u');
            user.timestamp = ++m_stamp;
            m_harness.session().appendMessage(user);
            AssistantMessage reply;
            reply.content.push_back(TextContent{std::string(1200, 'a'), std::nullopt});
            reply.stopReason = StopReason::Stop;
            reply.timestamp = ++m_stamp;
            m_harness.session().appendMessage(reply);
        }
        rebuild();
    }

    SessionHarness m_harness;
    FakeSettingsManager m_settings{Json{{"retry", Json{{"enabled", true}, {"maxRetries", 2}, {"baseDelayMs", 10}}},
                                        {"compaction", Json{{"keepRecentTokens", 500}, {"reserveTokens", 1000}}}}};
    FakeModelRuntime m_models;
    FakeResourceLoader m_resources;
    ToolRegistry m_tools;
    PosixFileSystem m_files;
    PosixProcessRunner m_runner;
    BoringCrypto m_crypto;
    SystemEnvironment m_environment;
    BashCommandExecutor m_bash{m_runner, m_files, m_crypto, m_environment};
    Model m_model;
    std::shared_ptr<ScriptedTool> m_readTool;
    std::unique_ptr<AgentSession> m_session;
    std::vector<AgentSessionEvent> m_events;
    std::int64_t m_stamp = 1000;
};

TEST_F(AgentSessionTest, PromptRunsATurnPersistsItAndSettles) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello there"));
    const auto result = m_session->prompt("hi", {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, PromptDisposition::Started);
    EXPECT_EQ(m_session->lastAssistantText(), "hello there");
    EXPECT_TRUE(m_session->isIdle());
    EXPECT_FALSE(m_session->isStreaming());
    const auto types = entryTypes();
    EXPECT_EQ(std::ranges::count(types, "message"), 3);  // system prompt, user, assistant
    ASSERT_EQ(eventsOf(SessionEventType::AgentEnd).size(), 1U);
    EXPECT_FALSE(eventsOf(SessionEventType::AgentEnd)[0].willRetry);
    ASSERT_EQ(eventsOf(SessionEventType::AgentSettled).size(), 1U);
    EXPECT_EQ(m_events.back().type, SessionEventType::AgentSettled);
    EXPECT_NE(m_session->systemPrompt().find("read: Read file contents"), std::string::npos);
}

TEST_F(AgentSessionTest, PromptNeedsModelAndCredentials) {
    m_models.setAuthenticated("faux", false);
    const auto result = m_session->prompt("hi", {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "no_auth");
    EXPECT_NE(result.error().message.find("faux"), std::string::npos);
    EXPECT_EQ(m_harness.session().entryCount(), 0U);
}

TEST_F(AgentSessionTest, TransientFailureIsRetriedAndFailedAttemptLeavesContext) {
    m_harness.provider().enqueue(failure("503 overloaded"));
    m_harness.provider().enqueue(m_harness.provider().textResponse("second try"));
    ASSERT_TRUE(m_session->prompt("hi", {}).has_value());
    EXPECT_EQ(m_session->lastAssistantText(), "second try");
    EXPECT_EQ(m_harness.provider().callCount(), 2);
    EXPECT_EQ(m_harness.sleeper().delays(), (std::vector<std::int64_t>{10}));
    ASSERT_EQ(eventsOf(SessionEventType::AutoRetryStart).size(), 1U);
    const auto ends = eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_TRUE(ends[0].success);
    const auto agentEnds = eventsOf(SessionEventType::AgentEnd);
    ASSERT_FALSE(agentEnds.empty());
    EXPECT_TRUE(agentEnds.front().willRetry);
    EXPECT_EQ(std::ranges::count(entryTypes(), "context_edit"), 1);
    // The retried request did not see the failed attempt.
    for (const auto& message : m_session->messages()) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            EXPECT_NE(assistant->stopReason, StopReason::Error);
        }
    }
}

TEST_F(AgentSessionTest, RetriesStopAfterTheBudget) {
    m_harness.provider().enqueue(failure("503 overloaded"));
    m_harness.provider().enqueue(failure("503 overloaded"));
    m_harness.provider().enqueue(failure("503 overloaded"));
    ASSERT_TRUE(m_session->prompt("hi", {}).has_value());
    EXPECT_EQ(m_harness.provider().callCount(), 3);
    const auto ends = eventsOf(SessionEventType::AutoRetryEnd);
    ASSERT_EQ(ends.size(), 1U);
    EXPECT_FALSE(ends[0].success);
    EXPECT_EQ(ends[0].attempt, 2);
    EXPECT_EQ(m_session->retryAttempt(), 0);
}

TEST_F(AgentSessionTest, ContextOverflowCompactsAndRunsTheTurnAgain) {
    seedHistory(4);
    m_harness.provider().enqueue(failure("prompt is too long: 300000 tokens > 200000 maximum"));
    m_harness.provider().enqueue(m_harness.provider().textResponse("## Goal\nsummary"));
    m_harness.provider().enqueue(m_harness.provider().textResponse("recovered"));
    ASSERT_TRUE(m_session->prompt("keep going", {}).has_value());
    EXPECT_EQ(m_session->lastAssistantText(), "recovered");
    const auto compactionEnds = eventsOf(SessionEventType::CompactionEnd);
    ASSERT_EQ(compactionEnds.size(), 1U);
    EXPECT_EQ(compactionEnds[0].reason, "overflow");
    EXPECT_TRUE(compactionEnds[0].willRetry);
    EXPECT_TRUE(compactionEnds[0].result.has_value());
    EXPECT_EQ(std::ranges::count(entryTypes(), "compaction"), 1);
    EXPECT_EQ(m_harness.provider().callCount(), 3);
}

TEST_F(AgentSessionTest, ManualCompactionThroughTheSession) {
    seedHistory(4);
    m_harness.provider().enqueue(m_harness.provider().textResponse("manual summary"));
    const auto result = m_session->compact(std::nullopt);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(std::ranges::count(entryTypes(), "compaction"), 1);
    EXPECT_FALSE(m_session->isCompacting());
    EXPECT_EQ(eventsOf(SessionEventType::CompactionStart).front().reason, "manual");
}

TEST_F(AgentSessionTest, PromptWhileStreamingNeedsABehaviorAndQueuesOtherwise) {
    Result<PromptDisposition> plain = PromptDisposition::Started;
    Result<PromptDisposition> queued = PromptDisposition::Started;
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions&, const Model&) {
        plain = m_session->prompt("no behavior", {});
        PromptOptions options;
        options.streamingBehavior = StreamingBehavior::FollowUp;
        queued = m_session->prompt("later", options);
        EXPECT_EQ(m_session->queued().followUp, (std::vector<std::string>{"later"}));
        return m_harness.provider().textResponse("first");
    });
    m_harness.provider().enqueue(m_harness.provider().textResponse("second"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error().code, "agent_busy");
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(*queued, PromptDisposition::Queued);
    EXPECT_EQ(m_session->lastAssistantText(), "second");
    EXPECT_EQ(m_harness.provider().callCount(), 2);
    EXPECT_TRUE(m_session->queued().followUp.empty());
    EXPECT_GE(eventsOf(SessionEventType::QueueUpdate).size(), 2U);
}

TEST_F(AgentSessionTest, AbortDuringARunStopsFurtherWork) {
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions&, const Model&) {
        m_session->abort();
        return failure("503 overloaded");
    });
    m_harness.provider().enqueue(m_harness.provider().textResponse("must not run"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    EXPECT_EQ(m_harness.provider().callCount(), 1);
    EXPECT_TRUE(m_session->isIdle());
    EXPECT_TRUE(eventsOf(SessionEventType::AutoRetryStart).empty());
}

TEST_F(AgentSessionTest, CustomMessagesAppendNowDeferOrRideWithTheNextPrompt) {
    ASSERT_TRUE(m_session->sendCustomMessage("note", Json("idle"), true, Json(), {}).has_value());
    EXPECT_EQ(std::ranges::count(entryTypes(), "custom_message"), 1);

    SendMessageOptions nextTurn;
    nextTurn.deliverAs = DeliverAs::NextTurn;
    ASSERT_TRUE(m_session->sendCustomMessage("aside", Json("later"), false, Json(), nextTurn).has_value());
    EXPECT_EQ(std::ranges::count(entryTypes(), "custom_message"), 1);
    m_harness.provider().enqueue(m_harness.provider().textResponse("ok"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    EXPECT_EQ(std::ranges::count(entryTypes(), "custom_message"), 2);
}

TEST_F(AgentSessionTest, ThinkingModelToolsAndSessionNameOperations) {
    m_session->setSessionName("my chat");
    EXPECT_EQ(m_session->sessionName(), "my chat");
    ASSERT_EQ(eventsOf(SessionEventType::SessionInfoChanged).size(), 1U);
    EXPECT_EQ(eventsOf(SessionEventType::SessionInfoChanged)[0].name, "my chat");

    EXPECT_FALSE(m_session->supportsThinking());
    m_session->setThinkingLevel(ThinkingLevel::High, false);
    EXPECT_EQ(m_session->thinkingLevel(), ThinkingLevel::Off);

    const auto tools = m_session->allTools();
    ASSERT_EQ(tools.size(), 1U);
    EXPECT_EQ(tools[0].name, "read");
    EXPECT_TRUE(tools[0].active);
    m_session->setActiveToolsByName({});
    EXPECT_TRUE(m_session->activeToolNames().empty());
    EXPECT_FALSE(m_session->allTools()[0].active);

    m_session->setAutoCompactionEnabled(false);
    EXPECT_FALSE(m_session->autoCompactionEnabled());
    m_session->setAutoRetryEnabled(false);
    EXPECT_FALSE(m_session->autoRetryEnabled());
    m_session->setSteeringMode(QueueMode::All);
    EXPECT_EQ(m_settings.view().steeringMode(), "all");
}

TEST_F(AgentSessionTest, BashCommandsAreRecordedAndStatsCountEverything) {
    const auto bash = m_session->executeBash("echo hi", nullptr, false, std::nullopt);
    ASSERT_TRUE(bash.has_value());
    EXPECT_EQ(bash->output, "hi\n");
    m_harness.provider().enqueue(m_harness.provider().textResponse("ok"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    const SessionStats stats = m_session->stats();
    EXPECT_EQ(stats.userMessages, 1);
    EXPECT_EQ(stats.assistantMessages, 1);
    EXPECT_EQ(stats.sessionId, m_harness.session().sessionId());
    EXPECT_TRUE(stats.contextUsage.has_value());
}

TEST_F(AgentSessionTest, NavigationIsRefusedWhileBusyAndWorksWhenIdle) {
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions&, const Model&) {
        const auto refused = m_session->navigateTree("anything", {});
        EXPECT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().code, "busy");
        return m_harness.provider().textResponse("ok");
    });
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    const auto forkable = m_session->forkableMessages();
    ASSERT_EQ(forkable.size(), 1U);
    const auto navigated = m_session->navigateTree(forkable[0].entryId, {});
    ASSERT_TRUE(navigated.has_value());
    EXPECT_EQ(navigated->editorText, "go");
}

TEST_F(AgentSessionTest, ReloadReloadsResources) {
    ASSERT_TRUE(m_session->reload().has_value());
    EXPECT_EQ(m_resources.reloads(), 1);
}

TEST_F(AgentSessionTest, DisposedSessionStopsDeliveringEvents) {
    m_session->dispose();
    const std::size_t before = m_events.size();
    m_session->setSessionName("x");
    EXPECT_EQ(m_events.size(), before);
}
