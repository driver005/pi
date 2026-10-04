#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.chat_completions_request_builder;

class ChatCompletionsRequestBuilderTest : public testing::Test {
protected:
    Model model(const std::string& provider = "openai",
                const std::string& baseUrl = "https://api.openai.com/v1") {
        Model m;
        m.id = "gpt-test";
        m.api = "openai-completions";
        m.provider = provider;
        m.baseUrl = baseUrl;
        m.maxTokens = 4000;
        m.contextWindow = 100000;
        m.input = {"text", "image"};
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
        t.description = "d";
        t.parameters = Json::parse(R"({"type":"object","properties":{}})");
        return t;
    }

    TranscriptContext context(std::vector<Message> messages, const std::string& prompt = "sys",
                              std::vector<Tool> tools = {}) {
        Context raw;
        raw.systemPrompt = prompt;
        if (!tools.empty()) {
            raw.tools = tools;
        }
        raw.messages = std::move(messages);
        return m_normalizer.normalizeContext(raw);
    }

    Json build(const Model& m, const TranscriptContext& c, const StreamOptions& o = {}) {
        return m_builder.build(m, c, o, 1);
    }

    AssistantMessage assistantFrom(const Model& m) {
        AssistantMessage a;
        a.api = m.api;
        a.provider = m.provider;
        a.model = m.id;
        return a;
    }

    TranscriptNormalizer m_normalizer;
    ChatCompletionsRequestBuilder m_builder;
};

TEST_F(ChatCompletionsRequestBuilderTest, BasicShapeForOpenAi) {
    StreamOptions options;
    options.sessionId = "sess-1";
    options.temperature = 0.2;
    const Json body = build(model(), context({user("hi")}), options);
    EXPECT_EQ(body["model"], "gpt-test");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["stream_options"]["include_usage"], true);
    EXPECT_EQ(body["store"], false);
    EXPECT_EQ(body["max_completion_tokens"], 4000);
    EXPECT_EQ(body["temperature"], 0.2);
    EXPECT_EQ(body["prompt_cache_key"], "sess-1");
    ASSERT_EQ(body["messages"].size(), 2U);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][0]["content"], "sys");
    EXPECT_EQ(body["messages"][1]["content"], "hi");
    EXPECT_FALSE(body.contains("tools"));
}

TEST_F(ChatCompletionsRequestBuilderTest, ReasoningModelUsesDeveloperRoleAndEffort) {
    Model m = model();
    m.reasoning = true;
    m.thinkingLevelMap = Json::parse(R"({"minimal":null})");
    StreamOptions options;
    options.reasoning = ThinkingLevel::Minimal;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["messages"][0]["role"], "developer");
    // minimal is unsupported, clamped up to low.
    EXPECT_EQ(body["reasoning_effort"], "low");
}

TEST_F(ChatCompletionsRequestBuilderTest, NonStandardProviderUsesMaxTokensAndNoStore) {
    const Json body = build(model("deepseek", "https://api.deepseek.com"), context({user("hi")}));
    EXPECT_EQ(body["max_tokens"], 4000);
    EXPECT_FALSE(body.contains("store"));
    EXPECT_FALSE(body.contains("max_completion_tokens"));
    EXPECT_FALSE(body.contains("prompt_cache_key"));
}

TEST_F(ChatCompletionsRequestBuilderTest, ToolsAndToolHistory) {
    Model m = model();
    AssistantMessage assistant = assistantFrom(m);
    ToolCall call;
    call.id = "call_1";
    call.name = "read";
    call.arguments = Json::parse(R"({"path":"a"})");
    assistant.content.emplace_back(TextContent{"looking", std::nullopt});
    assistant.content.emplace_back(call);
    ToolResultMessage result;
    result.toolCallId = "call_1";
    result.toolName = "read";
    result.content.emplace_back(TextContent{"file text", std::nullopt});
    const Json body = build(m, context({user("go"), assistant, result}, "sys", {tool("read")}));
    ASSERT_EQ(body["tools"].size(), 1U);
    EXPECT_EQ(body["tools"][0]["type"], "function");
    EXPECT_EQ(body["tools"][0]["function"]["name"], "read");
    EXPECT_FALSE(body["tools"][0]["function"].contains("strict"));
    const Json& messages = body["messages"];
    ASSERT_EQ(messages.size(), 4U);
    EXPECT_EQ(messages[2]["role"], "assistant");
    EXPECT_EQ(messages[2]["content"], "looking");
    EXPECT_EQ(messages[2]["tool_calls"][0]["function"]["arguments"], R"({"path":"a"})");
    EXPECT_EQ(messages[3]["role"], "tool");
    EXPECT_EQ(messages[3]["tool_call_id"], "call_1");
    EXPECT_EQ(messages[3]["content"], "file text");
}

TEST_F(ChatCompletionsRequestBuilderTest, EmptyToolsArrayWhenHistoryHasToolCalls) {
    Model m = model();
    AssistantMessage assistant = assistantFrom(m);
    ToolCall call;
    call.id = "c";
    call.name = "x";
    assistant.content.emplace_back(call);
    ToolResultMessage result;
    result.toolCallId = "c";
    result.content.emplace_back(TextContent{"r", std::nullopt});
    const Json body = build(m, context({user("go"), assistant, result}));
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_TRUE(body["tools"].empty());
}

TEST_F(ChatCompletionsRequestBuilderTest, ToolResultImagesBecomeFollowUpUserMessage) {
    Model m = model();
    AssistantMessage assistant = assistantFrom(m);
    ToolCall call;
    call.id = "c";
    call.name = "x";
    assistant.content.emplace_back(call);
    ToolResultMessage result;
    result.toolCallId = "c";
    result.content.emplace_back(ImageContent{"AAA", "image/png"});
    const Json body = build(m, context({user("go"), assistant, result}));
    const Json& messages = body["messages"];
    EXPECT_EQ(messages[3]["content"], "(see attached image)");
    EXPECT_EQ(messages[4]["role"], "user");
    EXPECT_EQ(messages[4]["content"][1]["image_url"]["url"], "data:image/png;base64,AAA");
}

TEST_F(ChatCompletionsRequestBuilderTest, ThinkingReplayedInRecordedField) {
    Model m = model("llamacpp", "http://localhost:8080/v1");
    AssistantMessage assistant = assistantFrom(m);
    ThinkingContent thinking;
    thinking.thinking = "reasoned";
    thinking.thinkingSignature = "reasoning_content";
    assistant.content.emplace_back(thinking);
    assistant.content.emplace_back(TextContent{"answer", std::nullopt});
    const Json body = build(m, context({user("go"), assistant, user("more")}));
    const Json& replayed = body["messages"][2];
    EXPECT_EQ(replayed["reasoning_content"], "reasoned");
    EXPECT_EQ(replayed["content"], "answer");
}

TEST_F(ChatCompletionsRequestBuilderTest, EmptyAssistantMessagesSkipped) {
    Model m = model();
    AssistantMessage assistant = assistantFrom(m);
    const Json body = build(m, context({user("go"), assistant, user("again")}));
    EXPECT_EQ(body["messages"].size(), 3U);
}

TEST_F(ChatCompletionsRequestBuilderTest, PipeToolCallIdsFolded) {
    Model m = model();
    EXPECT_EQ(m_builder.normalizeToolCallId(m, "call_1|fc_2"), "call_1_fc_2");
    const std::string longId = "call_1|" + std::string(200, 'x');
    const std::string folded = m_builder.normalizeToolCallId(m, longId);
    EXPECT_LE(folded.size(), 40U);
    EXPECT_EQ(folded.rfind("call_1_", 0), 0U);
    EXPECT_EQ(m_builder.normalizeToolCallId(m, std::string(60, 'a')).size(), 40U);
    Model other = model("groq", "https://api.groq.com/openai/v1");
    EXPECT_EQ(m_builder.normalizeToolCallId(other, std::string(60, 'a')).size(), 60U);
}

TEST_F(ChatCompletionsRequestBuilderTest, OpenRouterAnthropicCacheControlAndReasoning) {
    Model m = model("openrouter", "https://openrouter.ai/api/v1");
    m.id = "anthropic/claude";
    m.reasoning = true;
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    const Json body = build(m, context({user("hi")}, "sys", {tool("read")}), options);
    EXPECT_EQ(body["reasoning"]["effort"], "high");
    EXPECT_EQ(body["messages"][0]["content"][0]["cache_control"]["type"], "ephemeral");
    EXPECT_EQ(body["tools"][0]["cache_control"]["type"], "ephemeral");
    EXPECT_EQ(body["messages"][1]["content"][0]["cache_control"]["type"], "ephemeral");
}

TEST_F(ChatCompletionsRequestBuilderTest, ZaiThinkingFormat) {
    Model m = model("zai", "https://api.z.ai/api/paas/v4");
    m.reasoning = true;
    const Json off = build(m, context({user("hi")}));
    EXPECT_EQ(off["thinking"]["type"], "disabled");
    StreamOptions options;
    options.reasoning = ThinkingLevel::Medium;
    const Json on = build(m, context({user("hi")}), options);
    EXPECT_EQ(on["thinking"]["type"], "enabled");
    EXPECT_EQ(on["thinking"]["clear_thinking"], false);
}

TEST_F(ChatCompletionsRequestBuilderTest, ChatTemplateKwargsWithVariables) {
    Model m = model("local", "http://localhost:1234/v1");
    m.reasoning = true;
    m.compat = Json::parse(R"({"thinkingFormat":"chat-template","chatTemplateKwargs":{"enable_thinking":{"$var":"thinking.enabled"},"budget":{"$var":"thinking.budget"},"fixed":7}})");
    StreamOptions options;
    options.reasoning = ThinkingLevel::Low;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["chat_template_kwargs"]["enable_thinking"], true);
    EXPECT_EQ(body["chat_template_kwargs"]["budget"], 2048);
    EXPECT_EQ(body["chat_template_kwargs"]["fixed"], 7);
}

TEST_F(ChatCompletionsRequestBuilderTest, SamplingParamsOverrideLast) {
    Model m = model();
    m.samplingParams = Json::parse(R"({"top_p":0.5,"temperature":0.9})");
    StreamOptions options;
    options.temperature = 0.1;
    options.samplingParams = Json::parse(R"({"top_p":0.7})");
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["top_p"], 0.7);
    EXPECT_EQ(body["temperature"], 0.9);
}

TEST_F(ChatCompletionsRequestBuilderTest, ThinkingBudgetFieldCapsReasoning) {
    Model m = model("local", "http://localhost/v1");
    m.reasoning = true;
    m.compat = Json::parse(R"({"supportsThinkingTokenBudget":true})");
    m.maxTokens = 3000;
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    const Json body = build(m, context({user("hi")}), options);
    EXPECT_EQ(body["thinking_token_budget"], 3000 - 1024);
}
