#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.session.agent_session;
import pi.support.hook_bus;
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
        config.hooks = m_hookBus;
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

    IHookBus* m_hookBus = nullptr;
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

TEST_F(AgentSessionTest, DispositionCallbackFiresForStartedAndQueuedPrompts) {
    std::vector<PromptDisposition> seen;
    PromptOptions options;
    options.onDisposition = [&](PromptDisposition disposition) { seen.push_back(disposition); };
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions&, const Model&) {
        PromptOptions queued = options;
        queued.streamingBehavior = StreamingBehavior::Steer;
        m_session->prompt("steer me", queued);
        return m_harness.provider().textResponse("first");
    });
    m_harness.provider().enqueue(m_harness.provider().textResponse("second"));
    ASSERT_TRUE(m_session->prompt("go", options).has_value());
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0], PromptDisposition::Started);
    EXPECT_EQ(seen[1], PromptDisposition::Queued);
    // Rejected prompts never report a disposition.
    seen.clear();
    m_models.setAuthenticated("faux", false);
    EXPECT_FALSE(m_session->prompt("again", options).has_value());
    EXPECT_TRUE(seen.empty());
}

TEST_F(AgentSessionTest, TreeAccessorsAndQueueModesAndSlashCommands) {
    LoadedResources resources;
    PromptTemplate prompt;
    prompt.name = "ship";
    prompt.description = "Ship it";
    resources.promptTemplates.push_back(prompt);
    Skill skill;
    skill.name = "pdf";
    skill.description = "PDFs";
    resources.skills.push_back(skill);
    m_resources.set(resources);
    const auto commands = m_session->slashCommands();
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands[0].name, "ship");
    EXPECT_EQ(commands[0].source, "prompt");
    EXPECT_EQ(commands[1].name, "skill:pdf");
    EXPECT_EQ(commands[1].source, "skill");

    EXPECT_FALSE(m_session->leafId().has_value());
    m_harness.provider().enqueue(m_harness.provider().textResponse("ok"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    EXPECT_TRUE(m_session->leafId().has_value());
    EXPECT_EQ(m_session->entries().size(), m_harness.session().entryCount());
    ASSERT_EQ(m_session->tree().size(), 1U);

    EXPECT_EQ(m_session->steeringMode(), QueueMode::OneAtATime);
    m_session->setFollowUpMode(QueueMode::All);
    EXPECT_EQ(m_session->followUpMode(), QueueMode::All);
    EXPECT_EQ(m_session->pendingMessageCount(), 0U);
}

TEST_F(AgentSessionTest, PluginHooksBlockToolCallsAndSeeAgentEvents) {
    HookBus bus;
    m_hookBus = &bus;
    rebuild();
    std::vector<std::string> seen;
    bus.subscribe("tool_call", [](const std::string&, const Json& payload) -> Result<Json> {
        EXPECT_EQ(payload["toolName"], "read");
        return Json{{"block", true}, {"reason", "blocked by plugin"}};
    });
    for (const char* event : {"agent_start", "agent_end", "message_end"}) {
        bus.subscribe(event, [&seen](const std::string& name, const Json&) -> Result<Json> {
            seen.push_back(name);
            return Json();
        });
    }
    m_harness.provider().enqueue(m_harness.provider().toolCallResponse("read", Json{{"arg", "x"}}, "call-1"));
    m_harness.provider().enqueue(m_harness.provider().textResponse("done"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    EXPECT_EQ(m_readTool->executions(), 0);
    EXPECT_EQ(m_session->lastAssistantText(), "done");
    EXPECT_NE(std::ranges::find(seen, "agent_start"), seen.end());
    EXPECT_NE(std::ranges::find(seen, "message_end"), seen.end());
    EXPECT_NE(std::ranges::find(seen, "agent_end"), seen.end());
}

TEST_F(AgentSessionTest, PluginHooksCanRewriteToolResultsAndArguments) {
    HookBus bus;
    m_hookBus = &bus;
    rebuild();
    bus.subscribe("tool_call", [](const std::string&, const Json&) -> Result<Json> {
        return Json{{"input", Json{{"arg", "patched"}}}};
    });
    Json resultPayload;
    bus.subscribe("tool_result", [&resultPayload](const std::string&, const Json& payload) -> Result<Json> {
        resultPayload = payload;
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "rewritten"}}})}};
    });
    m_harness.provider().enqueue(m_harness.provider().toolCallResponse("read", Json{{"arg", "x"}}, "call-1"));
    m_harness.provider().enqueue(m_harness.provider().textResponse("done"));
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    EXPECT_EQ(m_readTool->executions(), 1);
    EXPECT_EQ(resultPayload["input"]["arg"], "patched");
    bool rewritten = false;
    for (const AgentMessage& message : m_session->messages()) {
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            rewritten = std::get<TextContent>(result->content.at(0)).text == "rewritten";
        }
    }
    EXPECT_TRUE(rewritten);
}

TEST_F(AgentSessionTest, AnInputPluginCanSwallowOrRewriteThePrompt) {
    HookBus bus;
    m_hookBus = &bus;
    rebuild();
    Json seen;
    const auto id = bus.subscribe("input", [&seen](const std::string&, const Json& payload) -> Result<Json> {
        seen = payload;
        return Json{{"action", "handled"}};
    });
    PromptOptions options;
    std::vector<PromptDisposition> dispositions;
    options.onDisposition = [&](PromptDisposition disposition) { dispositions.push_back(disposition); };
    const auto handled = m_session->prompt("swallow me", options);
    ASSERT_TRUE(handled.has_value());
    EXPECT_EQ(*handled, PromptDisposition::Handled);
    EXPECT_EQ(dispositions, std::vector<PromptDisposition>{PromptDisposition::Handled});
    EXPECT_EQ(m_harness.provider().callCount(), 0);
    EXPECT_EQ(seen["text"], "swallow me");
    EXPECT_EQ(seen["source"], "rpc");
    EXPECT_FALSE(seen.contains("streamingBehavior"));
    EXPECT_TRUE(m_session->messages().empty());

    bus.unsubscribe(id);
    bus.subscribe("input", [](const std::string&, const Json&) -> Result<Json> {
        return Json{{"action", "transform"}, {"text", "rewritten"}};
    });
    m_harness.provider().enqueue(m_harness.provider().textResponse("ok"));
    const auto started = m_session->prompt("original", {});
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(*started, PromptDisposition::Started);
    const std::vector<AgentMessage> messages = m_session->messages();
    bool found = false;
    for (const AgentMessage& message : messages) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            found = found || std::get<TextContent>(std::get<std::vector<UserContentBlock>>(user->content).at(0)).text == "rewritten";
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(AgentSessionTest, BeforeAgentStartAddsMessagesAndReplacesTheSystemPromptForTheTurn) {
    HookBus bus;
    m_hookBus = &bus;
    rebuild();
    const auto handler = bus.subscribe("before_agent_start", [](const std::string&, const Json& payload) -> Result<Json> {
        EXPECT_EQ(payload["prompt"], "go");
        EXPECT_NE(payload["systemPrompt"].get<std::string>().find("coding assistant"), std::string::npos);
        return Json{{"message", Json{{"customType", "note"}, {"content", "remember this"}}}, {"systemPrompt", "You are a pirate."}};
    });
    std::vector<TranscriptContext> contexts;
    const auto capture = [this, &contexts](const TranscriptContext& context, const StreamOptions&, const Model&) {
        contexts.push_back(context);
        return m_harness.provider().textResponse("arr");
    };
    m_harness.provider().enqueue(capture);
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    ASSERT_EQ(contexts.size(), 1U);
    const auto* system = std::get_if<SystemMessage>(&contexts[0].messages.front());
    ASSERT_TRUE(system != nullptr);
    ASSERT_TRUE(std::holds_alternative<std::string>(system->content));
    EXPECT_EQ(std::get<std::string>(system->content), "You are a pirate.");
    std::size_t systemCount = 0;
    for (const Message& message : contexts[0].messages) {
        systemCount += std::holds_alternative<SystemMessage>(message) ? 1U : 0U;
    }
    EXPECT_EQ(systemCount, 1U);
    bool noted = false;
    for (const AgentMessage& message : m_session->messages()) {
        if (const auto* custom = std::get_if<CustomMessage>(&message)) {
            noted = noted || custom->data.value("customType", "") == "note";
        }
    }
    EXPECT_TRUE(noted);

    // The replacement lasts one turn: without the plugin the next prompt renders the prompt again.
    bus.unsubscribe(handler);
    m_harness.provider().enqueue(capture);
    ASSERT_TRUE(m_session->prompt("again", {}).has_value());
    ASSERT_EQ(contexts.size(), 2U);
    bool pirate = false;
    for (const Message& message : contexts[1].messages) {
        if (const auto* entry = std::get_if<SystemMessage>(&message)) {
            pirate = pirate || (std::holds_alternative<std::string>(entry->content) && std::get<std::string>(entry->content) == "You are a pirate.");
        }
    }
    EXPECT_FALSE(pirate);
}

TEST_F(AgentSessionTest, BeforeProviderRequestPluginsSeeAndReplaceThePayload) {
    HookBus bus;
    m_hookBus = &bus;
    rebuild();
    bus.subscribe("before_provider_request", [](const std::string&, const Json& event) -> Result<Json> {
        Json payload = event["payload"];
        payload["metadata"] = "tagged";
        return payload;
    });
    std::optional<Json> replaced;
    m_harness.provider().enqueue([&](const TranscriptContext&, const StreamOptions& options, const Model& model) {
        replaced = options.onPayload ? options.onPayload(Json{{"model", "faux-1"}}, model) : std::nullopt;
        return m_harness.provider().textResponse("ok");
    });
    ASSERT_TRUE(m_session->prompt("go", {}).has_value());
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ((*replaced)["model"], "faux-1");
    EXPECT_EQ((*replaced)["metadata"], "tagged");
}
