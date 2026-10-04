#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.hook_bus;
import pi.support.plugin_hook_dispatcher;

class PluginHookDispatcherTest : public testing::Test {
protected:
    ToolCallContext toolContext(const Json& args) {
        m_call.id = "call-1";
        m_call.name = "bash";
        m_call.arguments = args;
        ToolCallContext context;
        context.toolCall = &m_call;
        context.args = args;
        return context;
    }

    void on(const std::string& event, const std::function<Json(const Json&)>& handler) {
        m_bus.subscribe(event, [handler](const std::string&, const Json& payload) -> Result<Json> { return handler(payload); });
    }

    ToolCall m_call;
    HookBus m_bus;
    PluginHookDispatcher m_dispatcher{m_bus};
};

TEST_F(PluginHookDispatcherTest, NoSubscribersMeansNoOpinion) {
    EXPECT_FALSE(m_dispatcher.beforeToolCall(toolContext(Json{{"command", "ls"}})).has_value());
    ToolCallContext after = toolContext(Json::object());
    after.result = AgentToolResult{};
    EXPECT_FALSE(m_dispatcher.afterToolCall(after).has_value());
    EXPECT_TRUE(m_dispatcher.transformContext({}).empty());
}

TEST_F(PluginHookDispatcherTest, ToolCallCanBlock) {
    Json seen;
    on("tool_call", [&seen](const Json& payload) {
        seen = payload;
        return Json{{"block", true}, {"reason", "no shell"}, {"terminate", true}};
    });
    const auto result = m_dispatcher.beforeToolCall(toolContext(Json{{"command", "ls"}}));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->block);
    EXPECT_EQ(*result->reason, "no shell");
    EXPECT_TRUE(result->terminate);
    EXPECT_EQ(seen["toolCallId"], "call-1");
    EXPECT_EQ(seen["toolName"], "bash");
    EXPECT_EQ(seen["input"]["command"], "ls");
}

TEST_F(PluginHookDispatcherTest, ToolCallInputPatchesChainAcrossHandlers) {
    on("tool_call", [](const Json&) { return Json{{"input", Json{{"command", "ls -a"}}}}; });
    std::string second;
    on("tool_call", [&second](const Json& payload) {
        second = payload["input"]["command"].get<std::string>();
        return Json();
    });
    const auto result = m_dispatcher.beforeToolCall(toolContext(Json{{"command", "ls"}}));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->block);
    EXPECT_EQ(result->args["command"], "ls -a");
    EXPECT_EQ(second, "ls -a");
}

TEST_F(PluginHookDispatcherTest, ToolCallWithoutChangesReturnsNothing) {
    on("tool_call", [](const Json&) { return Json{{"note", "fine"}}; });
    EXPECT_FALSE(m_dispatcher.beforeToolCall(toolContext(Json{{"a", 1}})).has_value());
}

TEST_F(PluginHookDispatcherTest, ToolResultHandlersCanRewriteTheResult) {
    on("tool_result", [](const Json& payload) {
        EXPECT_EQ(payload["content"][0]["text"], "secret");
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "[redacted]"}}})}, {"isError", true}};
    });
    ToolCallContext context = toolContext(Json::object());
    AgentToolResult original;
    original.content.push_back(TextContent{"secret", std::nullopt});
    original.structuredContent = Json{{"old", true}};
    context.result = original;
    const auto result = m_dispatcher.afterToolCall(context);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->content.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content->at(0)).text, "[redacted]");
    EXPECT_TRUE(result->structuredContent.is_null());
    ASSERT_TRUE(result->isError.has_value());
    EXPECT_TRUE(*result->isError);
}

TEST_F(PluginHookDispatcherTest, ToolResultCanKeepStructuredContentWhenReplacingContent) {
    on("tool_result", [](const Json&) {
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", "x"}}})}, {"structuredContent", Json{{"new", 1}}}};
    });
    ToolCallContext context = toolContext(Json::object());
    context.result = AgentToolResult{};
    const auto result = m_dispatcher.afterToolCall(context);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent["new"], 1);
}

TEST_F(PluginHookDispatcherTest, ContextHandlersReplaceMessages) {
    on("context", [](const Json& payload) {
        Json messages = payload["messages"];
        messages.push_back(Json{{"role", "user"}, {"content", "added"}, {"timestamp", 1}});
        return Json{{"messages", messages}};
    });
    UserMessage user;
    user.content = std::string("hello");
    user.timestamp = 5;
    const auto result = m_dispatcher.transformContext({AgentMessage(user)});
    ASSERT_EQ(result.size(), 2U);
}

TEST_F(PluginHookDispatcherTest, NotifyFiresEventsByType) {
    Json seen;
    on("message_end", [&seen](const Json& payload) {
        seen = payload;
        return Json();
    });
    m_dispatcher.notify(Json{{"type", "message_end"}, {"x", 1}});
    m_dispatcher.notify(Json{{"type", "turn_end"}});
    m_dispatcher.notify(Json::array());
    EXPECT_EQ(seen["x"], 1);
}

TEST_F(PluginHookDispatcherTest, ContextWithSystemSeesTheWholeTranscriptAndCanReplaceIt) {
    TranscriptContext context;
    SystemMessage system;
    system.content = "be brief";
    context.messages.push_back(system);
    UserMessage user;
    TextContent block;
    block.text = "hi";
    user.content = std::vector<UserContentBlock>{block};
    context.messages.push_back(user);

    EXPECT_EQ(m_dispatcher.transformFinalContext(context).messages.size(), 2U) << "no subscribers: unchanged";

    Json seen;
    on("context_with_system", [&seen](const Json& payload) {
        seen = payload;
        Json messages = payload["messages"];
        messages[0]["content"] = "be verbose";
        return Json{{"messages", messages}};
    });
    const TranscriptContext changed = m_dispatcher.transformFinalContext(context);
    ASSERT_EQ(seen["messages"].size(), 2U);
    EXPECT_EQ(seen["messages"][0]["role"], "system");
    ASSERT_EQ(changed.messages.size(), 2U);
    EXPECT_EQ(std::get<std::string>(std::get<SystemMessage>(changed.messages[0]).content), "be verbose");

    on("context_with_system", [](const Json&) { return Json{{"messages", Json::array({Json{{"role", "bogus"}}})}}; });
    EXPECT_EQ(m_dispatcher.transformFinalContext(context).messages.size(), 2U) << "an invalid replacement is ignored";
}
