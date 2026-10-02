#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.anthropic_request_builder;

class AnthropicRequestBuilderTest : public testing::Test {
protected:
    Model model() {
        Model m;
        m.id = "claude-sonnet-4";
        m.api = "anthropic-messages";
        m.provider = "anthropic";
        m.input = {"text", "image"};
        m.maxTokens = 32000;
        m.contextWindow = 200000;
        return m;
    }

    UserMessage user(const std::string& text) {
        UserMessage u;
        u.content = text;
        return u;
    }

    Tool tool(const std::string& name) {
        Tool t;
        t.name = name;
        t.description = "desc " + name;
        t.parameters = Json::parse(
            R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
        return t;
    }

    TranscriptContext context(std::vector<Message> messages, const std::string& prompt = "be nice",
                              std::vector<Tool> tools = {}) {
        Context raw;
        raw.systemPrompt = prompt;
        if (!tools.empty()) {
            raw.tools = tools;
        }
        raw.messages = std::move(messages);
        return m_normalizer.normalizeContext(raw);
    }

    Json build(const Model& m, const TranscriptContext& c, const StreamOptions& o = {},
               bool oauth = false) {
        return m_builder.build(m, c, oauth, o, 1);
    }

    TranscriptNormalizer m_normalizer;
    AnthropicRequestBuilder m_builder;
};

TEST_F(AnthropicRequestBuilderTest, BasicRequestShape) {
    const Json body = build(model(), context({user("hello")}));
    EXPECT_EQ(body["model"], "claude-sonnet-4");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["max_tokens"], 32000);
    EXPECT_EQ(body["system"][0]["text"], "be nice");
    EXPECT_EQ(body["system"][0]["cache_control"]["type"], "ephemeral");
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    // The last user message gets a cache breakpoint, which turns a string into blocks.
    EXPECT_EQ(body["messages"][0]["content"][0]["text"], "hello");
    EXPECT_EQ(body["messages"][0]["content"][0]["cache_control"]["type"], "ephemeral");
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("thinking"));
}

TEST_F(AnthropicRequestBuilderTest, CacheRetentionNoneOmitsCacheControl) {
    StreamOptions options;
    options.cacheRetention = "none";
    const Json body = build(model(), context({user("hello")}), options);
    EXPECT_FALSE(body["system"][0].contains("cache_control"));
    EXPECT_TRUE(body["messages"][0]["content"].is_string());
}

TEST_F(AnthropicRequestBuilderTest, LongRetentionUsesOneHourTtl) {
    StreamOptions options;
    options.cacheRetention = "long";
    const Json body = build(model(), context({user("hello")}), options);
    EXPECT_EQ(body["system"][0]["cache_control"]["ttl"], "1h");
}

TEST_F(AnthropicRequestBuilderTest, ConvertsToolsWithCacheOnLast) {
    const Json body = build(model(), context({user("hi")}, "p", {tool("read"), tool("write")}));
    ASSERT_EQ(body["tools"].size(), 2U);
    EXPECT_EQ(body["tools"][0]["name"], "read");
    EXPECT_EQ(body["tools"][0]["eager_input_streaming"], true);
    EXPECT_EQ(body["tools"][0]["input_schema"]["required"][0], "path");
    EXPECT_FALSE(body["tools"][0].contains("cache_control"));
    EXPECT_TRUE(body["tools"][1].contains("cache_control"));
}

TEST_F(AnthropicRequestBuilderTest, ToolHistoryAndResultsGrouped) {
    AssistantMessage assistant;
    assistant.api = "anthropic-messages";
    assistant.provider = "anthropic";
    assistant.model = "claude-sonnet-4";
    ToolCall first;
    first.id = "t1";
    first.name = "read";
    first.arguments = Json::parse(R"({"path":"a"})");
    ToolCall second = first;
    second.id = "t2";
    assistant.content.emplace_back(first);
    assistant.content.emplace_back(second);
    assistant.stopReason = StopReason::ToolUse;
    ToolResultMessage r1;
    r1.toolCallId = "t1";
    r1.toolName = "read";
    r1.content.emplace_back(TextContent{"one", std::nullopt});
    ToolResultMessage r2 = r1;
    r2.toolCallId = "t2";
    r2.isError = true;
    r2.content = {TextContent{"two", std::nullopt}};
    const Json body = build(model(), context({user("go"), assistant, r1, r2}));
    ASSERT_EQ(body["messages"].size(), 3U);
    EXPECT_EQ(body["messages"][1]["content"][1]["type"], "tool_use");
    const Json& results = body["messages"][2]["content"];
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0]["tool_use_id"], "t1");
    EXPECT_EQ(results[0]["content"], "one");
    EXPECT_EQ(results[1]["is_error"], true);
    EXPECT_TRUE(results[1].contains("cache_control"));
}

TEST_F(AnthropicRequestBuilderTest, ImageToolResultGetsPlaceholderText) {
    AssistantMessage assistant;
    assistant.api = "anthropic-messages";
    assistant.provider = "anthropic";
    assistant.model = "claude-sonnet-4";
    ToolCall call;
    call.id = "t1";
    call.name = "read";
    assistant.content.emplace_back(call);
    ToolResultMessage result;
    result.toolCallId = "t1";
    result.content.emplace_back(ImageContent{"AAAA", "image/png"});
    const Json body = build(model(), context({user("go"), assistant, result}));
    const Json& content = body["messages"][2]["content"][0]["content"];
    ASSERT_EQ(content.size(), 2U);
    EXPECT_EQ(content[0]["text"], "(see attached image)");
    EXPECT_EQ(content[1]["source"]["media_type"], "image/png");
}

TEST_F(AnthropicRequestBuilderTest, BudgetThinking) {
    Model m = model();
    m.reasoning = true;
    StreamOptions options;
    options.reasoning = ThinkingLevel::Medium;
    options.maxTokens = 4000;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["thinking"]["type"], "enabled");
    EXPECT_EQ(body["thinking"]["budget_tokens"], 8192);
    EXPECT_EQ(body["thinking"]["display"], "summarized");
    EXPECT_EQ(body["max_tokens"], 12192);
    EXPECT_FALSE(body.contains("temperature"));
    const auto betas = m_builder.betaFeatures(m, context({user("hi")}), false, options);
    ASSERT_EQ(betas.size(), 1U);
    EXPECT_EQ(betas[0], "interleaved-thinking-2025-05-14");
}

TEST_F(AnthropicRequestBuilderTest, AdaptiveThinkingUsesEffort) {
    Model m = model();
    m.reasoning = true;
    m.compat = Json::parse(R"({"forceAdaptiveThinking":true})");
    m.thinkingLevelMap = Json::parse(R"({"xhigh":"xhigh"})");
    StreamOptions options;
    options.reasoning = ThinkingLevel::XHigh;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["thinking"]["type"], "adaptive");
    EXPECT_EQ(body["output_config"]["effort"], "xhigh");
    EXPECT_TRUE(m_builder.betaFeatures(m, context({user("hi")}), false, options).empty());
}

TEST_F(AnthropicRequestBuilderTest, ReasoningModelWithoutRequestDisablesThinking) {
    Model m = model();
    m.reasoning = true;
    StreamOptions options;
    options.temperature = 0.5;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["thinking"]["type"], "disabled");
    EXPECT_EQ(body["temperature"], 0.5);
}

TEST_F(AnthropicRequestBuilderTest, OAuthAddsIdentityAndClaudeCodeToolNames) {
    const Json body = build(model(), context({user("hi")}, "mine", {tool("read")}), {}, true);
    ASSERT_EQ(body["system"].size(), 2U);
    EXPECT_EQ(body["system"][0]["text"], "You are Claude Code, Anthropic's official CLI for Claude.");
    EXPECT_EQ(body["system"][1]["text"], "mine");
    EXPECT_EQ(body["tools"][0]["name"], "Read");
    EXPECT_EQ(m_builder.fromClaudeCodeName("Read", {tool("read")}), "read");
    const auto betas = m_builder.betaFeatures(model(), context({user("hi")}), true, {});
    EXPECT_EQ(betas.front(), "claude-code-20250219");
    EXPECT_TRUE(m_builder.isOAuthToken("sk-ant-oat01-abc"));
    EXPECT_FALSE(m_builder.isOAuthToken("sk-ant-api03-abc"));
}

TEST_F(AnthropicRequestBuilderTest, ConfiguredBetaHeaderOverridesDefaults) {
    Model m = model();
    m.headers = {{"Anthropic-Beta", "a, b ,a"}};
    const auto betas = m_builder.betaFeatures(m, context({user("hi")}), true, {});
    ASSERT_EQ(betas.size(), 2U);
    EXPECT_EQ(betas[1], "b");
    StreamOptions options;
    options.headers = {{"anthropic-beta", std::nullopt}};
    EXPECT_TRUE(m_builder.betaFeatures(m, context({user("hi")}), true, options).empty());
}

TEST_F(AnthropicRequestBuilderTest, UnsignedThinkingReplayedAsText) {
    AssistantMessage assistant;
    assistant.api = "anthropic-messages";
    assistant.provider = "anthropic";
    assistant.model = "claude-sonnet-4";
    ThinkingContent thinking;
    thinking.thinking = "hmm";
    assistant.content.emplace_back(thinking);
    assistant.content.emplace_back(TextContent{"answer", std::nullopt});
    const Json body = build(model(), context({user("go"), assistant, user("again")}));
    const Json& blocks = body["messages"][1]["content"];
    EXPECT_EQ(blocks[0]["type"], "text");
    EXPECT_EQ(blocks[0]["text"], "hmm");
}

TEST_F(AnthropicRequestBuilderTest, ToolCallIdsSanitizedAcrossModels) {
    AssistantMessage assistant;
    assistant.api = "openai-responses";
    assistant.provider = "openai";
    assistant.model = "gpt";
    ToolCall call;
    call.id = "call|abc";
    call.name = "read";
    assistant.content.emplace_back(call);
    ToolResultMessage result;
    result.toolCallId = "call|abc";
    result.content.emplace_back(TextContent{"ok", std::nullopt});
    const Json body = build(model(), context({user("go"), assistant, result}));
    EXPECT_EQ(body["messages"][1]["content"][0]["id"], "call_abc");
    EXPECT_EQ(body["messages"][2]["content"][0]["tool_use_id"], "call_abc");
}
