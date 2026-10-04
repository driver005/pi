#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.hook_bus;
import pi.support.plugin_hook_extension;
import pi.support.tool_bridge;
import pi.testing.durable_harness_fixture;
import pi.testing.scripted_tool;

class NullHookApi : public IHookApi {
public:
    std::int64_t taskId() const override {
        return 1;
    }
    std::int64_t conversationId() const override {
        return 1;
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
};

class PluginHookExtensionTest : public testing::Test {
protected:
    void on(const std::string& event, const std::function<Json(const Json&)>& handler) {
        m_bus->subscribe(event, [handler](const std::string&, const Json& payload) -> Result<Json> { return handler(payload); });
    }

    /** Runs the extension's hook `name` of task `task`. */
    std::optional<Json> hook(const std::string& task, const std::string& name, const Json& payload) {
        const Extension extension = PluginHookExtension(m_bus).extension("plugin-hooks");
        for (const HookRegistration& registration : extension.hooks) {
            if (registration.task == task && registration.handlers.contains(name)) {
                auto decision = registration.handlers.at(name)(payload, m_api);
                EXPECT_TRUE(decision.has_value());
                return decision ? *decision : std::nullopt;
            }
        }
        ADD_FAILURE() << "no hook " << name;
        return std::nullopt;
    }

    std::shared_ptr<HookBus> m_bus = std::make_shared<HookBus>();
    NullHookApi m_api;
};

TEST_F(PluginHookExtensionTest, WithoutSubscribersEveryHookIsANoOp) {
    const Json call = Json::object({{"id", "c"}, {"name", "echo"}, {"arguments", Json::object()}});
    EXPECT_FALSE(hook("pi.tool", "beforeTool", call).has_value());
    EXPECT_FALSE(hook("pi.tool", "afterTool", Json::object({{"call", call}, {"result", Json::object()}})).has_value());
    EXPECT_FALSE(hook("pi.generation", "beforeRequest", Json::object({{"messages", Json::array()}})).has_value());
    EXPECT_FALSE(hook("pi.generation", "afterResponse", Json::object()).has_value());
}

TEST_F(PluginHookExtensionTest, ToolCallHandlersCanBlockWithAReason) {
    on("tool_call", [](const Json&) { return Json::object({{"block", true}, {"reason", "nope"}}); });
    const Json call = Json::object({{"id", "c"}, {"name", "bash"}, {"arguments", Json::object({{"command", "rm"}})}});
    EXPECT_EQ(hook("pi.tool", "beforeTool", call), std::optional<Json>(Json::object({{"block", "nope"}})));
}

TEST_F(PluginHookExtensionTest, ToolCallHandlersSeeEachOthersArgumentChanges) {
    Json seen;
    on("tool_call", [](const Json&) { return Json::object({{"input", Json::object({{"command", "ls"}})}}); });
    on("tool_call", [&seen](const Json& payload) {
        seen = payload;
        return Json();
    });
    const Json call = Json::object({{"id", "c"}, {"name", "bash"}, {"arguments", Json::object({{"command", "rm"}})}});
    EXPECT_EQ(hook("pi.tool", "beforeTool", call), std::optional<Json>(Json::object({{"arguments", Json::object({{"command", "ls"}})}})));
    EXPECT_EQ(seen, Json::parse(R"({"toolCallId":"c","toolName":"bash","input":{"command":"ls"}})"));
}

TEST_F(PluginHookExtensionTest, ToolResultHandlersReplaceParts) {
    on("tool_result", [](const Json& payload) {
        EXPECT_EQ(payload.at("toolName"), "echo");
        return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", "changed"}})})}, {"isError", true}});
    });
    const Json call = Json::object({{"id", "c"}, {"name", "echo"}, {"arguments", Json::object()}});
    const Json result = Json::object({{"content", Json::array()}, {"diagnostics", Json::array()}});
    const auto decision = hook("pi.tool", "afterTool", Json::object({{"call", call}, {"result", result}}));
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->at("content")[0].at("text"), "changed");
    EXPECT_EQ(decision->at("isError"), true);
    EXPECT_TRUE(decision->contains("diagnostics"));
}

TEST_F(PluginHookExtensionTest, ContextHandlersReplaceTheRequestMessages) {
    on("context", [](const Json& payload) {
        Json messages = payload.at("messages");
        messages.push_back(Json::object({{"role", "user"}, {"content", "extra"}}));
        return Json::object({{"messages", messages}});
    });
    const auto decision = hook("pi.generation", "beforeRequest", Json::object({{"messages", Json::array()}}));
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->at("messages").size(), 1u);
}

TEST_F(PluginHookExtensionTest, ResponsesAndToolRoundsAreObservedAsMessageAndTurnEnds) {
    std::vector<std::string> seen;
    on("message_end", [&seen](const Json& event) {
        seen.push_back(event.at("type").get<std::string>() + ":" + event.at("message").at("role").get<std::string>());
        return Json();
    });
    on("turn_end", [&seen](const Json& event) {
        seen.push_back(event.at("type").get<std::string>() + ":" + std::to_string(event.at("toolResults").size()));
        return Json();
    });
    hook("pi.generation", "afterResponse", Json::object({{"role", "assistant"}}));
    hook("pi.generation", "afterTools", Json::object({{"assistant", Json::object({{"role", "assistant"}})}, {"results", Json::array({1})}}));
    EXPECT_EQ(seen, (std::vector<std::string>{"message_end:assistant", "turn_end:1"}));
}

TEST_F(PluginHookExtensionTest, ProviderPayloadsAreReplacedByHandlers) {
    EXPECT_FALSE(hook("pi.generation", "beforeProviderRequest", Json::object({{"payload", Json::object()}})).has_value());
    on("before_provider_request", [](const Json& event) {
        Json payload = event.at("payload");
        payload["temperature"] = 0;
        return payload;
    });
    const auto decision = hook("pi.generation", "beforeProviderRequest", Json::object({{"payload", Json::object({{"model", "m"}})}}));
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->at("payload").at("model"), "m");
    EXPECT_EQ(decision->at("payload").at("temperature"), 0);
}

TEST_F(PluginHookExtensionTest, CompactionsCanBeDeclinedOrSummarisedByAPlugin) {
    Json seen;
    const Json compaction = Json::parse(R"({"reason":"threshold","entries":[{"id":1}],"messages":[{"role":"user"}],"firstKept":7,"instructions":"focus"})");
    EXPECT_FALSE(hook("pi.compaction", "beforeCompact", compaction).has_value());
    on("session_before_compact", [&seen](const Json& event) {
        seen = event;
        return Json::object({{"compaction", Json::object({{"summary", "from plugin"}, {"firstKeptEntryId", "ignored"}})}});
    });
    const auto summarised = hook("pi.compaction", "beforeCompact", compaction);
    ASSERT_TRUE(summarised.has_value());
    EXPECT_EQ(*summarised, Json::object({{"summary", "from plugin"}}));
    EXPECT_EQ(seen.at("reason"), "threshold");
    EXPECT_EQ(seen.at("customInstructions"), "focus");
    EXPECT_EQ(seen.at("preparation").at("firstKeptEntryId"), 7);
    EXPECT_EQ(seen.at("preparation").at("messagesToSummarize").size(), 1u);
    EXPECT_EQ(seen.at("branchEntries").size(), 1u);
    EXPECT_EQ(seen.at("willRetry"), false);
    on("session_before_compact", [](const Json&) { return Json::object({{"cancel", true}}); });
    EXPECT_EQ(hook("pi.compaction", "beforeCompact", compaction), std::optional<Json>(Json::object({{"decline", true}})));
}

class PluginHooksInARunTest : public testing::Test {
protected:
    PluginHooksInARunTest()
        : m_tool(std::make_shared<ScriptedTool>("echo", "pong")),
          m_cache(std::make_shared<ToolSetCache>([this](const std::string&) { return ToolSetCache::ToolSet{m_tool}; })) {
        EXPECT_TRUE(m_fixture.registry().install(ToolBridge(m_cache, "/default").extension("coding-tools")).has_value());
        EXPECT_TRUE(m_fixture.registry().install(PluginHookExtension(m_bus).extension("plugin-hooks")).has_value());
        EXPECT_TRUE(m_fixture.open().has_value());
    }

    Json run() {
        m_fixture.faux().enqueue(m_fixture.faux().toolCallResponse("echo", Json::object({{"arg", "x"}}), "call-1"));
        m_fixture.faux().enqueue(m_fixture.faux().textResponse("done"));
        auto submission = m_fixture.root()->submit(m_fixture.input("go"));
        EXPECT_TRUE(submission.has_value());
        EXPECT_TRUE((*submission)->wait().has_value());
        EXPECT_TRUE(m_fixture.root()->waitForIdle().has_value());
        auto context = m_fixture.root()->context();
        EXPECT_TRUE(context.has_value());
        return context ? *context : Json::object();
    }

    Json toolResult(const Json& context) {
        for (const Json& message : context.at("messages")) {
            if (message.value("role", std::string()) == "toolResult") {
                return message;
            }
        }
        ADD_FAILURE() << "no tool result";
        return Json::object();
    }

    std::shared_ptr<HookBus> m_bus = std::make_shared<HookBus>();
    std::shared_ptr<ScriptedTool> m_tool;
    std::shared_ptr<ToolSetCache> m_cache;
    DurableHarnessFixture m_fixture;
};

TEST_F(PluginHooksInARunTest, ABlockedCallDoesNotRunTheToolAndTheModelSeesWhy) {
    m_bus->subscribe("tool_call", [](const std::string&, const Json&) -> Result<Json> { return Json::object({{"block", true}, {"reason", "not today"}}); });
    const Json result = toolResult(run());
    EXPECT_EQ(m_tool->executions(), 0);
    EXPECT_EQ(result.at("isError"), true);
    EXPECT_NE(result.at("content").dump().find("not today"), std::string::npos);
}

TEST_F(PluginHooksInARunTest, AToolResultCanBeRewrittenBeforeTheModelSeesIt) {
    m_bus->subscribe("tool_result", [](const std::string&, const Json&) -> Result<Json> {
        return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", "redacted"}})})}});
    });
    const Json result = toolResult(run());
    EXPECT_EQ(m_tool->executions(), 1);
    EXPECT_EQ(result.at("content")[0].at("text"), "redacted");
}

TEST_F(PluginHooksInARunTest, ObserversSeeTheRunsMessagesAndTurns) {
    std::mutex mutex;
    std::vector<std::string> seen;
    for (const std::string event : {"message_end", "turn_end"}) {
        m_bus->subscribe(event, [&, event](const std::string&, const Json&) -> Result<Json> {
            const std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(event);
            return Json();
        });
    }
    run();
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), "message_end"), 2);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), "turn_end"), 1);
}

TEST_F(PluginHooksInARunTest, ProviderRequestPayloadsPassThroughThePlugins) {
    m_bus->subscribe("before_provider_request", [](const std::string&, const Json& event) -> Result<Json> {
        Json payload = event.at("payload");
        payload["metadata"] = "tagged";
        return payload;
    });
    std::mutex mutex;
    std::vector<Json> replaced;
    m_fixture.models().setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        if (options.onPayload) {
            if (auto changed = options.onPayload(Json::object({{"model", "faux-1"}}), model)) {
                const std::lock_guard<std::mutex> lock(mutex);
                replaced.push_back(*changed);
            }
        }
        return m_fixture.faux().stream(model, context, options);
    });
    run();
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(replaced.size(), 2u);
    EXPECT_EQ(replaced[0].at("metadata"), "tagged");
    EXPECT_EQ(replaced[0].at("model"), "faux-1");
}
