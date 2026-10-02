#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.codex_request_builder;

class CodexRequestBuilderTest : public testing::Test {
protected:
    Model model(bool reasoning = true) {
        Model m;
        m.id = "gpt-5-codex";
        m.api = "openai-codex-responses";
        m.provider = "openai-codex";
        m.baseUrl = "https://chatgpt.com/backend-api";
        m.reasoning = reasoning;
        m.input = {"text"};
        return m;
    }

    TranscriptContext context(const std::string& prompt, std::vector<Tool> tools = {}) {
        UserMessage user;
        user.content = "hi";
        Context raw;
        raw.systemPrompt = prompt;
        if (!tools.empty()) {
            raw.tools = tools;
        }
        raw.messages.emplace_back(user);
        return m_normalizer.normalizeContext(raw);
    }

    Tool tool() {
        Tool t;
        t.name = "read";
        t.description = "d";
        t.parameters = Json::parse(R"({"type":"object","properties":{}})");
        return t;
    }

    TranscriptNormalizer m_normalizer;
    CodexRequestBuilder m_builder;
};

TEST_F(CodexRequestBuilderTest, SystemPromptBecomesInstructions) {
    StreamOptions options;
    options.sessionId = "s1";
    options.temperature = 0.1;
    const Json body = m_builder.build(model(), context("be brief"), options, 1);
    EXPECT_EQ(body["instructions"], "be brief");
    ASSERT_EQ(body["input"].size(), 1U);
    EXPECT_EQ(body["input"][0]["role"], "user");
    EXPECT_EQ(body["store"], false);
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["text"], (Json{{"verbosity", "low"}}));
    EXPECT_EQ(body["include"], Json::array({"reasoning.encrypted_content"}));
    EXPECT_EQ(body["prompt_cache_key"], "s1");
    EXPECT_EQ(body["tool_choice"], "auto");
    EXPECT_EQ(body["parallel_tool_calls"], true);
    EXPECT_EQ(body["temperature"], 0.1);
    EXPECT_FALSE(body.contains("max_output_tokens"));
}

TEST_F(CodexRequestBuilderTest, EmptyPromptGetsADefault) {
    EXPECT_EQ(m_builder.build(model(), context(""), {}, 1)["instructions"], "You are a helpful assistant.");
}

TEST_F(CodexRequestBuilderTest, CacheKeyFollowsRetention) {
    StreamOptions options;
    options.sessionId = "s1";
    options.cacheRetention = "none";
    EXPECT_FALSE(m_builder.build(model(), context("p"), options, 1).contains("prompt_cache_key"));
}

TEST_F(CodexRequestBuilderTest, ToolsCarryNullStrict) {
    StreamOptions options;
    options.toolChoice = "required";
    const Json body = m_builder.build(model(), context("p", {tool()}), options, 1);
    EXPECT_EQ(body["tool_choice"], "required");
    EXPECT_TRUE(body["tools"][0].contains("strict"));
    EXPECT_TRUE(body["tools"][0]["strict"].is_null());
    Model lax = model();
    lax.compat = Json::parse(R"({"supportsStrictMode":false})");
    EXPECT_FALSE(m_builder.build(lax, context("p", {tool()}), options, 1)["tools"][0].contains("strict"));
}

TEST_F(CodexRequestBuilderTest, ReasoningEffortAndOff) {
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    EXPECT_EQ(m_builder.build(model(), context("p"), options, 1)["reasoning"], (Json{{"effort", "high"}, {"summary", "auto"}}));
    EXPECT_EQ(m_builder.build(model(), context("p"), {}, 1)["reasoning"], (Json{{"effort", "none"}}));
    Model mapped = model();
    mapped.thinkingLevelMap = Json::parse(R"({"high":"xhigh","off":"minimal"})");
    EXPECT_EQ(m_builder.build(mapped, context("p"), options, 1)["reasoning"]["effort"], "xhigh");
    EXPECT_EQ(m_builder.build(mapped, context("p"), {}, 1)["reasoning"]["effort"], "minimal");
    mapped.thinkingLevelMap = Json::parse(R"({"off":null})");
    EXPECT_FALSE(m_builder.build(mapped, context("p"), {}, 1).contains("reasoning"));
    EXPECT_FALSE(m_builder.build(model(false), context("p"), {}, 1).contains("reasoning"));
}
