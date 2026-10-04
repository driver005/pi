#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mistral_request_builder;

class MistralRequestBuilderTest : public testing::Test {
protected:
    Model model(bool reasoning = false) {
        Model m;
        m.id = "mistral-large";
        m.api = "mistral-conversations";
        m.provider = "mistral";
        m.maxTokens = 4000;
        m.contextWindow = 100000;
        m.input = {"text", "image"};
        m.reasoning = reasoning;
        return m;
    }

    TextContent text(const std::string& value) {
        TextContent block;
        block.text = value;
        return block;
    }

    UserMessage user(const std::string& value) {
        UserMessage u;
        u.content = value;
        return u;
    }

    AssistantMessage assistant(const Model& m, std::vector<AssistantContentBlock> content) {
        AssistantMessage a;
        a.api = m.api;
        a.provider = m.provider;
        a.model = m.id;
        a.content = std::move(content);
        return a;
    }

    Tool tool(const std::string& name) {
        Tool t;
        t.name = name;
        t.description = "d";
        t.parameters = Json::parse(R"({"type":"object","properties":{}})");
        return t;
    }

    TranscriptContext context(std::vector<Message> messages, std::vector<Tool> tools = {}) {
        Context raw;
        raw.systemPrompt = "sys";
        if (!tools.empty()) {
            raw.tools = tools;
        }
        raw.messages = std::move(messages);
        return m_normalizer.normalizeContext(raw);
    }

    Json build(const Model& m, const TranscriptContext& c, const StreamOptions& o = {}) {
        return m_builder.build(m, c, o, 1);
    }

    TranscriptNormalizer m_normalizer;
    MistralRequestBuilder m_builder;
};

TEST_F(MistralRequestBuilderTest, BasicPayload) {
    StreamOptions options;
    options.temperature = 0.2;
    options.toolChoice = "required";
    const Json body = build(model(), context({user("hi")}, {tool("read")}), options);
    EXPECT_EQ(body["model"], "mistral-large");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["messages"][0], (Json{{"role", "system"}, {"content", "sys"}}));
    EXPECT_EQ(body["messages"][1], (Json{{"role", "user"}, {"content", "hi"}}));
    EXPECT_EQ(body["temperature"], 0.2);
    EXPECT_EQ(body["max_tokens"], 4000);
    EXPECT_EQ(body["tool_choice"], "required");
    EXPECT_EQ(body["tools"][0]["function"]["strict"], false);
    EXPECT_EQ(body["tools"][0]["function"]["name"], "read");
}

TEST_F(MistralRequestBuilderTest, UserImagesAreChunksOrOmitted) {
    UserMessage withImage;
    withImage.content = std::vector<UserContentBlock>{text("look"), ImageContent{"QUJD", "image/png"}};
    const Json body = build(model(), context({withImage}));
    EXPECT_EQ(body["messages"][1]["content"][1], (Json{{"type", "image_url"}, {"image_url", "data:image/png;base64,QUJD"}}));
    UserMessage onlyImage;
    onlyImage.content = std::vector<UserContentBlock>{ImageContent{"QUJD", "image/png"}};
    Model textOnly = model();
    textOnly.input = {"text"};
    // The transcript transformer already swaps images for a placeholder text block.
    const Json downgraded = build(textOnly, context({onlyImage}))["messages"][1]["content"];
    ASSERT_EQ(downgraded.size(), 1U);
    EXPECT_EQ(downgraded[0]["type"], "text");
}

TEST_F(MistralRequestBuilderTest, AssistantTurnWithThinkingTextAndCalls) {
    const Model m = model();
    ThinkingContent thought;
    thought.thinking = "hmm";
    ToolCall call;
    call.id = "abc123XYZ";
    call.name = "read";
    call.arguments = Json{{"p", 1}};
    const Json body = build(m, context({user("q"), assistant(m, {thought, text("answer"), text("  "), call})}));
    const Json& message = body["messages"][2];
    EXPECT_EQ(message["role"], "assistant");
    EXPECT_EQ(message["prefix"], false);
    ASSERT_EQ(message["content"].size(), 2U);
    EXPECT_EQ(message["content"][0], (Json{{"type", "thinking"}, {"thinking", Json::array({Json{{"type", "text"}, {"text", "hmm"}}})}}));
    EXPECT_EQ(message["tool_calls"][0], (Json{{"id", "abc123XYZ"},
                                              {"type", "function"},
                                              {"function", Json{{"name", "read"}, {"arguments", R"({"p":1})"}}},
                                              {"index", 0}}));
}

TEST_F(MistralRequestBuilderTest, ForeignToolCallIdsBecomeNineCharacterIds) {
    const Model m = model();
    Model other = m;
    other.provider = "anthropic";
    ToolCall call;
    call.id = "toolu_01ABCDEF";
    call.name = "read";
    ToolResultMessage result;
    result.toolCallId = "toolu_01ABCDEF";
    result.toolName = "read";
    result.content = {text("ok")};
    const Json body = build(m, context({user("q"), assistant(other, {call}), result}));
    const std::string id = body["messages"][2]["tool_calls"][0]["id"];
    EXPECT_EQ(id.size(), 9U);
    EXPECT_EQ(body["messages"][3]["tool_call_id"], id);
    EXPECT_EQ(body["messages"][3]["name"], "read");
}

TEST_F(MistralRequestBuilderTest, ToolResultText) {
    const Model m = model();
    ToolCall call;
    call.id = "abcdefghi";
    call.name = "read";
    ToolResultMessage failed;
    failed.toolCallId = "abcdefghi";
    failed.toolName = "read";
    failed.isError = true;
    failed.content = {text(" boom ")};
    EXPECT_EQ(build(m, context({user("q"), assistant(m, {call}), failed}))["messages"][3]["content"][0]["text"],
              "[tool error] boom");
    ToolResultMessage empty;
    empty.toolCallId = "abcdefghi";
    empty.toolName = "read";
    EXPECT_EQ(build(m, context({user("q"), assistant(m, {call}), empty}))["messages"][3]["content"][0]["text"],
              "(no tool output)");
    ToolResultMessage image;
    image.toolCallId = "abcdefghi";
    image.toolName = "read";
    image.content = {ImageContent{"QUJD", "image/png"}};
    const Json withImage = build(m, context({user("q"), assistant(m, {call}), image}));
    EXPECT_EQ(withImage["messages"][3]["content"][0]["text"], "(see attached image)");
    EXPECT_EQ(withImage["messages"][3]["content"][1]["type"], "image_url");
}

TEST_F(MistralRequestBuilderTest, ReasoningUsesEffortWhenAMapExists) {
    Model m = model(true);
    m.thinkingLevelMap = Json::parse(R"({"high":"max","off":"none"})");
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    EXPECT_EQ(build(m, context({user("q")}), options)["reasoning_effort"], "max");
    options.reasoning = ThinkingLevel::Medium;
    EXPECT_EQ(build(m, context({user("q")}), options)["reasoning_effort"], "high");
    EXPECT_EQ(build(m, context({user("q")}))["reasoning_effort"], "none");
    m.thinkingLevelMap = Json::object();
    EXPECT_FALSE(build(m, context({user("q")})).contains("reasoning_effort"));
}

TEST_F(MistralRequestBuilderTest, ReasoningModelsWithoutAMapUsePromptMode) {
    const Model m = model(true);
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    EXPECT_EQ(build(m, context({user("q")}), options)["prompt_mode"], "reasoning");
    EXPECT_FALSE(build(m, context({user("q")})).contains("prompt_mode"));
    EXPECT_FALSE(build(model(), context({user("q")}), options).contains("prompt_mode"));
}

TEST_F(MistralRequestBuilderTest, PromptCacheKeyFollowsRetention) {
    StreamOptions options;
    options.sessionId = "s1";
    EXPECT_EQ(build(model(), context({user("q")}), options)["prompt_cache_key"], "s1");
    EXPECT_TRUE(m_builder.usesPromptCaching(options));
    options.cacheRetention = "none";
    EXPECT_FALSE(build(model(), context({user("q")}), options).contains("prompt_cache_key"));
    EXPECT_FALSE(m_builder.usesPromptCaching({}));
}
