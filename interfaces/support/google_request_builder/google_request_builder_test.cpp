#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.google_request_builder;

class GoogleRequestBuilderTest : public testing::Test {
protected:
    Model model(const std::string& id = "gemini-2.5-flash", bool reasoning = false) {
        Model m;
        m.id = id;
        m.api = "google-generative-ai";
        m.provider = "google";
        m.baseUrl = "https://generativelanguage.googleapis.com/v1beta";
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
        auto body = m_builder.build(m, c, o, 1);
        EXPECT_TRUE(body.has_value());
        return body ? *body : Json();
    }

    TranscriptNormalizer m_normalizer;
    GoogleRequestBuilder m_builder;
};

TEST_F(GoogleRequestBuilderTest, BasicBody) {
    StreamOptions options;
    options.temperature = 0.3;
    const Json body = build(model(), context({user("hi")}), options);
    EXPECT_EQ(body["systemInstruction"], (Json{{"role", "user"}, {"parts", Json::array({Json{{"text", "sys"}}})}}));
    EXPECT_EQ(body["contents"], Json::array({Json{{"role", "user"}, {"parts", Json::array({Json{{"text", "hi"}}})}}}));
    EXPECT_EQ(body["generationConfig"]["temperature"], 0.3);
    EXPECT_EQ(body["generationConfig"]["maxOutputTokens"], 4000);
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body["generationConfig"].contains("thinkingConfig"));
}

TEST_F(GoogleRequestBuilderTest, ImagesAreInlineData) {
    UserMessage withImage;
    withImage.content = std::vector<UserContentBlock>{text("look"), ImageContent{"QUJD", "image/png"}};
    const Json body = build(model(), context({withImage}));
    EXPECT_EQ(body["contents"][0]["parts"][1], (Json{{"inlineData", Json{{"mimeType", "image/png"}, {"data", "QUJD"}}}}));
}

TEST_F(GoogleRequestBuilderTest, ToolsAndToolChoice) {
    StreamOptions options;
    options.toolChoice = "any";
    const Json body = build(model(), context({user("q")}, {tool("read")}), options);
    EXPECT_EQ(body["tools"][0]["functionDeclarations"][0]["name"], "read");
    EXPECT_EQ(body["tools"][0]["functionDeclarations"][0]["parametersJsonSchema"]["type"], "object");
    EXPECT_EQ(body["toolConfig"]["functionCallingConfig"]["mode"], "ANY");
    options.toolChoice = "none";
    EXPECT_EQ(build(model(), context({user("q")}, {tool("read")}), options)["toolConfig"]["functionCallingConfig"]["mode"],
              "NONE");
    EXPECT_FALSE(build(model(), context({user("q")}, {tool("read")}))["toolConfig"].is_object());
}

TEST_F(GoogleRequestBuilderTest, AssistantTurnsAndToolResultsMergeIntoOneUserTurn) {
    const Model m = model();
    ToolCall first;
    first.id = "a";
    first.name = "read";
    first.arguments = Json{{"p", 1}};
    ToolCall second;
    second.id = "b";
    second.name = "ls";
    ToolResultMessage r1;
    r1.toolCallId = "a";
    r1.toolName = "read";
    r1.content = {text("one")};
    ToolResultMessage r2;
    r2.toolCallId = "b";
    r2.toolName = "ls";
    r2.isError = true;
    r2.content = {text("bad")};
    const Json body = build(m, context({user("q"), assistant(m, {first, second}), r1, r2}));
    const Json& contents = body["contents"];
    ASSERT_EQ(contents.size(), 3U);
    EXPECT_EQ(contents[1]["role"], "model");
    EXPECT_EQ(contents[1]["parts"][0]["functionCall"], (Json{{"name", "read"}, {"args", Json{{"p", 1}}}}));
    ASSERT_EQ(contents[2]["parts"].size(), 2U);
    EXPECT_EQ(contents[2]["parts"][0]["functionResponse"]["response"], (Json{{"output", "one"}}));
    EXPECT_EQ(contents[2]["parts"][1]["functionResponse"]["response"], (Json{{"error", "bad"}}));
}

TEST_F(GoogleRequestBuilderTest, Gemini3NeedsToolCallIds) {
    const Model m = model("gemini-3-pro-preview");
    Model foreign = m;
    foreign.provider = "anthropic";
    ToolCall call;
    call.id = "weird id!";
    call.name = "read";
    ToolResultMessage result;
    result.toolCallId = "weird id!";
    result.toolName = "read";
    result.content = {text("ok")};
    const Json body = build(m, context({user("q"), assistant(foreign, {call}), result}));
    EXPECT_EQ(body["contents"][1]["parts"][0]["functionCall"]["id"], "weird_id_");
    EXPECT_EQ(body["contents"][2]["parts"][0]["functionResponse"]["id"], "weird_id_");
    Model older = model();
    Model olderForeign = older;
    olderForeign.provider = "anthropic";
    EXPECT_FALSE(build(older, context({user("q"), assistant(olderForeign, {call}), result}))["contents"][1]["parts"][0]
                         ["functionCall"]
                     .contains("id"));
}

TEST_F(GoogleRequestBuilderTest, ToolResultImagesNestForGemini3AndSplitForOlder) {
    ToolResultMessage result;
    result.toolCallId = "a";
    result.toolName = "read";
    result.content = {text("cap"), ImageContent{"QUJD", "image/png"}};
    ToolCall call;
    call.id = "a";
    call.name = "read";
    const Model g3 = model("gemini-3-flash");
    const Json nested = build(g3, context({user("q"), assistant(g3, {call}), result}));
    EXPECT_EQ(nested["contents"].size(), 3U);
    EXPECT_EQ(nested["contents"][2]["parts"][0]["functionResponse"]["parts"][0]["inlineData"]["data"], "QUJD");
    const Model g25 = model();
    const Json split = build(g25, context({user("q"), assistant(g25, {call}), result}));
    ASSERT_EQ(split["contents"].size(), 4U);
    EXPECT_EQ(split["contents"][3]["parts"][0]["text"], "Tool result image:");
}

TEST_F(GoogleRequestBuilderTest, ThoughtSignaturesOnlyForSameModelAndValidBase64) {
    const Model m = model("gemini-3-pro-preview", true);
    ThinkingContent thought;
    thought.thinking = "hmm";
    thought.thinkingSignature = "QUJDRA==";
    TextContent answer = text("ok");
    answer.textSignature = "not base64!";
    const Json body = build(m, context({user("q"), assistant(m, {thought, answer})}));
    const Json& parts = body["contents"][1]["parts"];
    ASSERT_EQ(parts.size(), 2U);
    EXPECT_EQ(parts[0], (Json{{"thought", true}, {"text", "hmm"}, {"thoughtSignature", "QUJDRA=="}}));
    EXPECT_EQ(parts[1], (Json{{"text", "ok"}}));

    Model other = m;
    other.id = "gemini-3-flash";
    const Json foreign = build(other, context({user("q"), assistant(m, {thought})}));
    EXPECT_EQ(foreign["contents"][1]["parts"][0], (Json{{"text", "hmm"}}));
}

TEST_F(GoogleRequestBuilderTest, EmptyBlocksAreDroppedUnlessSigned) {
    const Model m = model();
    ThinkingContent blank;
    blank.thinking = " ";
    const Json body = build(m, context({user("q"), assistant(m, {text("  "), blank})}));
    EXPECT_EQ(body["contents"].size(), 1U);
}

TEST_F(GoogleRequestBuilderTest, ThinkingBudgetForTwoPointFiveModels) {
    Model m = model("gemini-2.5-pro", true);
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    Json body = build(m, context({user("q")}), options);
    EXPECT_EQ(body["generationConfig"]["thinkingConfig"], (Json{{"includeThoughts", true}, {"thinkingBudget", 32768}}));
    options.thinkingBudgets = Json::parse(R"({"high":999})");
    EXPECT_EQ(build(m, context({user("q")}), options)["generationConfig"]["thinkingConfig"]["thinkingBudget"], 999);
    Model unknown = model("gemini-2.0-thinking", true);
    options.reasoning = ThinkingLevel::Medium;
    options.thinkingBudgets = Json();
    EXPECT_EQ(build(unknown, context({user("q")}), options)["generationConfig"]["thinkingConfig"]["thinkingBudget"], -1);
}

TEST_F(GoogleRequestBuilderTest, ThinkingLevelForGemini3) {
    const Model m = model("gemini-3.1-pro-preview", true);
    EXPECT_TRUE(m_builder.usesThinkingLevel(m));
    StreamOptions options;
    options.reasoning = ThinkingLevel::Low;
    EXPECT_EQ(build(m, context({user("q")}), options)["generationConfig"]["thinkingConfig"],
              (Json{{"includeThoughts", true}, {"thinkingLevel", "LOW"}}));
}

TEST_F(GoogleRequestBuilderTest, DisabledThinkingDependsOnTheWireFormat) {
    const Model budget = model("gemini-2.5-flash", true);
    EXPECT_EQ(build(budget, context({user("q")}))["generationConfig"]["thinkingConfig"], (Json{{"thinkingBudget", 0}}));
    const Model level = model("gemini-3-flash", true);
    EXPECT_EQ(build(level, context({user("q")}))["generationConfig"]["thinkingConfig"], (Json{{"thinkingBudget", 0}}));
    Model mapped = level;
    mapped.thinkingLevelMap = Json::parse(R"({"off":null})");
    EXPECT_EQ(build(mapped, context({user("q")}))["generationConfig"]["thinkingConfig"],
              (Json{{"thinkingLevel", "MINIMAL"}}));
}

TEST_F(GoogleRequestBuilderTest, UnusableThinkingMappingIsAnError) {
    Model m = model("gemini-3-pro", true);
    m.thinkingLevelMap = Json::parse(R"({"high":"extreme"})");
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    const auto body = m_builder.build(m, context({user("q")}), options, 1);
    ASSERT_FALSE(body.has_value());
    EXPECT_NE(body.error().message.find("Unsupported Google thinking level"), std::string::npos);
}

TEST_F(GoogleRequestBuilderTest, NonReasoningModelsGetNoThinkingConfig) {
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    EXPECT_FALSE(build(model(), context({user("q")}), options)["generationConfig"].contains("thinkingConfig"));
}
