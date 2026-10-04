#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.responses_request_builder;

class ResponsesRequestBuilderTest : public testing::Test {
protected:
    Model model(bool reasoning = false) {
        Model m;
        m.id = "gpt-test";
        m.api = "openai-responses";
        m.provider = "openai";
        m.baseUrl = "https://api.openai.com/v1";
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

    UserMessage user(const std::string& text) {
        UserMessage u;
        u.content = text;
        return u;
    }

    AssistantMessage assistant(const Model& m, std::vector<AssistantContentBlock> content) {
        AssistantMessage a;
        a.api = m.api;
        a.provider = m.provider;
        a.model = m.id;
        a.content = std::move(content);
        a.stopReason = StopReason::Stop;
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
    ResponsesRequestBuilder m_builder;
};

TEST_F(ResponsesRequestBuilderTest, BasicRequestShape) {
    const Json body = build(model(), context({user("hi")}));
    EXPECT_EQ(body["model"], "gpt-test");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["store"], false);
    ASSERT_EQ(body["input"].size(), 2U);
    EXPECT_EQ(body["input"][0], (Json{{"role", "system"}, {"content", "sys"}}));
    EXPECT_EQ(body["input"][1]["role"], "user");
    EXPECT_EQ(body["input"][1]["content"][0], (Json{{"type", "input_text"}, {"text", "hi"}}));
    EXPECT_EQ(body["max_output_tokens"], 4000);
    EXPECT_FALSE(body.contains("tools"));
}

TEST_F(ResponsesRequestBuilderTest, ReasoningModelsGetDeveloperInstructions) {
    const Json body = build(model(true), context({user("hi")}));
    EXPECT_EQ(body["input"][0]["role"], "developer");
    Model noDeveloper = model(true);
    noDeveloper.compat = Json::parse(R"({"supportsDeveloperRole":false})");
    EXPECT_EQ(build(noDeveloper, context({user("hi")}))["input"][0]["role"], "system");
}

TEST_F(ResponsesRequestBuilderTest, ImagesAndCacheKey) {
    UserMessage withImage;
    withImage.content = std::vector<UserContentBlock>{text("look"), ImageContent{"QUJD", "image/png"}};
    StreamOptions options;
    options.sessionId = "session-1";
    const Json body = build(model(), context({withImage}), options);
    EXPECT_EQ(body["input"][1]["content"][1],
              (Json{{"type", "input_image"}, {"detail", "auto"}, {"image_url", "data:image/png;base64,QUJD"}}));
    EXPECT_EQ(body["prompt_cache_key"], "session-1");
    EXPECT_FALSE(body.contains("prompt_cache_retention"));
    options.cacheRetention = "long";
    EXPECT_EQ(build(model(), context({withImage}), options)["prompt_cache_retention"], "24h");
    options.cacheRetention = "none";
    EXPECT_FALSE(build(model(), context({withImage}), options).contains("prompt_cache_key"));
}

TEST_F(ResponsesRequestBuilderTest, CacheKeyIsClampedToSixtyFourCharacters) {
    StreamOptions options;
    options.sessionId = std::string(100, 'a');
    EXPECT_EQ(build(model(), context({user("x")}), options)["prompt_cache_key"].get<std::string>().size(), 64U);
}

TEST_F(ResponsesRequestBuilderTest, AssistantItemsReplayWithSignatures) {
    const Model m = model(true);
    ThinkingContent thinking;
    thinking.thinking = "hmm";
    thinking.thinkingSignature = R"({"type":"reasoning","id":"rs_1","summary":[]})";
    TextContent answer;
    answer.text = "answer";
    answer.textSignature = R"({"v":1,"id":"msg_9","phase":"final_answer"})";
    ToolCall call;
    call.id = "call_1|fc_1";
    call.name = "read";
    call.arguments = Json{{"path", "a"}};
    ToolResultMessage result;
    result.toolCallId = "call_1|fc_1";
    result.toolName = "read";
    result.content = {text("ok")};
    const Json body = build(m, context({user("q"), assistant(m, {thinking, answer, call}), result}));
    const Json& input = body["input"];
    ASSERT_EQ(input.size(), 6U);
    EXPECT_EQ(input[2]["type"], "reasoning");
    EXPECT_EQ(input[3]["type"], "message");
    EXPECT_EQ(input[3]["id"], "msg_9");
    EXPECT_EQ(input[3]["phase"], "final_answer");
    EXPECT_EQ(input[3]["content"][0]["text"], "answer");
    EXPECT_EQ(input[4], (Json{{"type", "function_call"},
                              {"id", "fc_1"},
                              {"call_id", "call_1"},
                              {"name", "read"},
                              {"arguments", R"({"path":"a"})"}}));
    EXPECT_EQ(input[5], (Json{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "ok"}}));
}

TEST_F(ResponsesRequestBuilderTest, UnsignedTextGetsAFallbackId) {
    const Model m = model();
    const TextContent plain = text("plain");
    const Json body = build(m, context({user("q"), assistant(m, {plain})}));
    EXPECT_EQ(body["input"][2]["id"], "msg_pi_1");
    EXPECT_FALSE(body["input"][2].contains("phase"));
}

TEST_F(ResponsesRequestBuilderTest, ToolCallsFromAnotherModelLoseTheirItemId) {
    const Model m = model();
    Model other = m;
    other.id = "other-model";
    ToolCall call;
    call.id = "call_1|fc_1";
    call.name = "read";
    ToolResultMessage result;
    result.toolCallId = "call_1|fc_1";
    result.toolName = "read";
    result.content = {text("ok")};
    const Json body = build(m, context({user("q"), assistant(other, {call}), result}));
    EXPECT_FALSE(body["input"][2].contains("id"));
    EXPECT_EQ(body["input"][2]["call_id"], "call_1");
}

TEST_F(ResponsesRequestBuilderTest, ForeignProviderToolIdsAreRewrittenToFunctionCallIds) {
    const Model m = model();
    Model other = m;
    other.provider = "anthropic";
    other.api = "anthropic-messages";
    ToolCall call;
    call.id = "toolu_01|weird id!";
    call.name = "read";
    ToolResultMessage result;
    result.toolCallId = "toolu_01|weird id!";
    result.toolName = "read";
    result.content = {text("ok")};
    const Json body = build(m, context({user("q"), assistant(other, {call}), result}));
    const std::string callId = body["input"][2]["call_id"];
    EXPECT_EQ(callId, "toolu_01");
    EXPECT_EQ(body["input"][3]["call_id"], "toolu_01");
}

TEST_F(ResponsesRequestBuilderTest, ToolResultImagesAndEmptyOutput) {
    const Model m = model();
    ToolCall call;
    call.id = "c1|fc_1";
    call.name = "read";
    ToolResultMessage withImage;
    withImage.toolCallId = "c1|fc_1";
    withImage.toolName = "read";
    withImage.content = {text("cap"), ImageContent{"QUJD", "image/png"}};
    const Json body = build(m, context({user("q"), assistant(m, {call}), withImage}));
    const Json& output = body["input"][3]["output"];
    ASSERT_TRUE(output.is_array());
    EXPECT_EQ(output[0], (Json{{"type", "input_text"}, {"text", "cap"}}));
    EXPECT_EQ(output[1]["type"], "input_image");

    ToolResultMessage empty;
    empty.toolCallId = "c1|fc_1";
    empty.toolName = "read";
    const Json emptyBody = build(m, context({user("q"), assistant(m, {call}), empty}));
    EXPECT_EQ(emptyBody["input"][3]["output"], "(no tool output)");
}

TEST_F(ResponsesRequestBuilderTest, ToolsAndToolChoice) {
    StreamOptions options;
    options.toolChoice = "auto";
    const Json body = build(model(), context({user("q")}, {tool("read")}), options);
    EXPECT_EQ(body["tools"][0], (Json{{"type", "function"},
                                      {"name", "read"},
                                      {"description", "d"},
                                      {"parameters", Json::parse(R"({"type":"object","properties":{}})")}}));
    EXPECT_EQ(body["tool_choice"], "auto");
    Model strict = model();
    strict.compat = Json::parse(R"({"supportsStrictMode":true})");
    EXPECT_EQ(build(strict, context({user("q")}, {tool("read")}))["tools"][0]["strict"], false);
}

TEST_F(ResponsesRequestBuilderTest, ReasoningEffortAndSummary) {
    Model m = model(true);
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    Json body = build(m, context({user("q")}), options);
    EXPECT_EQ(body["reasoning"], (Json{{"effort", "high"}, {"summary", "auto"}}));
    EXPECT_EQ(body["include"], Json::array({"reasoning.encrypted_content"}));

    m.thinkingLevelMap = Json::parse(R"({"high":"max"})");
    EXPECT_EQ(build(m, context({user("q")}), options)["reasoning"]["effort"], "max");
}

TEST_F(ResponsesRequestBuilderTest, ReasoningOffSendsNoneUnlessHidden) {
    Model m = model(true);
    EXPECT_EQ(build(m, context({user("q")}))["reasoning"], (Json{{"effort", "none"}}));
    m.thinkingLevelMap = Json::parse(R"({"off":"minimal"})");
    EXPECT_EQ(build(m, context({user("q")}))["reasoning"]["effort"], "minimal");
    m.thinkingLevelMap = Json::parse(R"({"off":null})");
    EXPECT_FALSE(build(m, context({user("q")})).contains("reasoning"));
    Model copilot = model(true);
    copilot.provider = "github-copilot";
    EXPECT_FALSE(build(copilot, context({user("q")})).contains("reasoning"));
}

TEST_F(ResponsesRequestBuilderTest, MaxOutputTokensHasAFloorAndCanBeDisabled) {
    StreamOptions options;
    options.maxTokens = 5;
    EXPECT_EQ(build(model(), context({user("q")}), options)["max_output_tokens"], 16);
    Model off = model();
    off.compat = Json::parse(R"({"supportsMaxOutputTokens":false})");
    EXPECT_FALSE(build(off, context({user("q")}))
                     .contains("max_output_tokens"));
}

TEST_F(ResponsesRequestBuilderTest, ChatGptSignInOmitsRejectedFields) {
    StreamOptions options;
    options.apiKey = "eyJ-chatgpt-token";
    options.temperature = 0.5;
    options.cacheRetention = "long";
    options.sessionId = "s";
    const Json body = build(model(), context({user("q")}), options);
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("prompt_cache_retention"));
    EXPECT_TRUE(body.contains("prompt_cache_key"));
    options.apiKey = "sk-real";
    EXPECT_TRUE(build(model(), context({user("q")}), options).contains("temperature"));
}

TEST_F(ResponsesRequestBuilderTest, SamplingParamsOverrideNamedFields) {
    Model m = model();
    m.samplingParams = Json::parse(R"({"top_p":0.9,"temperature":0.1})");
    StreamOptions options;
    options.temperature = 0.7;
    options.samplingParams = Json::parse(R"({"top_p":0.5})");
    const Json body = build(m, context({user("q")}), options);
    EXPECT_EQ(body["top_p"], 0.5);
    EXPECT_EQ(body["temperature"], 0.1);
}
