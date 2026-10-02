#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.support.bedrock_request_builder;

class BedrockRequestBuilderTest : public testing::Test {
protected:
    Model model(const std::string& id = "anthropic.claude-sonnet-4-20250514-v1:0", bool reasoning = false) {
        Model m;
        m.id = id;
        m.name = "Claude";
        m.api = "bedrock-converse-stream";
        m.provider = "amazon-bedrock";
        m.maxTokens = 8000;
        m.contextWindow = 200000;
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

    Json build(const Model& m, const TranscriptContext& c, const StreamOptions& o = {}, const std::string& region = "us-east-1") {
        auto body = m_builder.build(m, c, o, region, 1);
        EXPECT_TRUE(body.has_value());
        return body ? *body : Json();
    }

    Base64Codec m_base64;
    TranscriptNormalizer m_normalizer;
    BedrockRequestBuilder m_builder{m_base64};
};

TEST_F(BedrockRequestBuilderTest, BasicBodyWithCaching) {
    StreamOptions options;
    options.temperature = 0.4;
    const Json body = build(model(), context({user("hi")}), options);
    EXPECT_EQ(body["system"][0], (Json{{"text", "sys"}}));
    EXPECT_EQ(body["system"][1], (Json{{"cachePoint", Json{{"type", "default"}}}}));
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"][0], (Json{{"text", "hi"}}));
    EXPECT_EQ(body["messages"][0]["content"][1], (Json{{"cachePoint", Json{{"type", "default"}}}}));
    EXPECT_EQ(body["inferenceConfig"], (Json{{"maxTokens", 8000}, {"temperature", 0.4}}));
    EXPECT_FALSE(body.contains("toolConfig"));
}

TEST_F(BedrockRequestBuilderTest, CachingOffAndLongRetention) {
    StreamOptions options;
    options.cacheRetention = "none";
    const Json none = build(model(), context({user("hi")}), options);
    EXPECT_EQ(none["system"].size(), 1U);
    EXPECT_EQ(none["messages"][0]["content"].size(), 1U);
    options.cacheRetention = "long";
    EXPECT_EQ(build(model(), context({user("hi")}), options)["system"][1]["cachePoint"]["ttl"], "1h");
}

TEST_F(BedrockRequestBuilderTest, NonClaudeModelsGetNoCachePointsUnlessForced) {
    Model nova = model("amazon.nova-pro-v1:0");
    nova.name = "Nova Pro";
    EXPECT_EQ(build(nova, context({user("hi")}))["system"].size(), 1U);
    StreamOptions options;
    options.env["AWS_BEDROCK_FORCE_CACHE"] = "1";
    EXPECT_EQ(build(nova, context({user("hi")}), options)["system"].size(), 2U);
}

TEST_F(BedrockRequestBuilderTest, ImagesAndEmptyTextPlaceholders) {
    UserMessage withImage;
    withImage.content = std::vector<UserContentBlock>{text("look"), ImageContent{"QUJD", "image/jpg"}};
    const Json body = build(model(), context({withImage}));
    EXPECT_EQ(body["messages"][0]["content"][1], (Json{{"image", Json{{"format", "jpeg"}, {"source", Json{{"bytes", "QUJD"}}}}}}));
    UserMessage blank;
    blank.content = std::vector<UserContentBlock>{text("  ")};
    StreamOptions options;
    options.cacheRetention = "none";
    EXPECT_EQ(build(model(), context({blank}), options)["messages"][0]["content"][0], (Json{{"text", "<empty>"}}));
    UserMessage odd;
    odd.content = std::vector<UserContentBlock>{ImageContent{"QUJD", "image/tiff"}};
    const auto failed = m_builder.build(model(), context({odd}), {}, "us-east-1", 1);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "Unknown image type: image/tiff");
}

TEST_F(BedrockRequestBuilderTest, ToolCallsToolResultsAndMerging) {
    const Model m = model();
    ToolCall first;
    first.id = "a";
    first.name = "read";
    first.arguments = Json{{"p", 1}, {"", "dropped"}};
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
    StreamOptions options;
    options.cacheRetention = "none";
    const Json body = build(m, context({user("q"), assistant(m, {first, second}), r1, r2}), options);
    const Json& messages = body["messages"];
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[1]["content"][0]["toolUse"], (Json{{"toolUseId", "a"}, {"name", "read"}, {"input", Json{{"p", 1}}}}));
    ASSERT_EQ(messages[2]["content"].size(), 2U);
    EXPECT_EQ(messages[2]["content"][0]["toolResult"]["status"], "success");
    EXPECT_EQ(messages[2]["content"][0]["toolResult"]["content"][0], (Json{{"text", "one"}}));
    EXPECT_EQ(messages[2]["content"][1]["toolResult"]["status"], "error");
    EXPECT_EQ(messages[2]["content"][1]["toolResult"]["content"][0], (Json{{"text", "<empty>"}}));
}

TEST_F(BedrockRequestBuilderTest, ToolConfigAndChoice) {
    StreamOptions options;
    options.toolChoice = "any";
    const Json body = build(model(), context({user("q")}, {tool("read")}), options);
    EXPECT_EQ(body["toolConfig"]["tools"][0]["toolSpec"]["name"], "read");
    EXPECT_EQ(body["toolConfig"]["tools"][0]["toolSpec"]["inputSchema"]["json"]["type"], "object");
    EXPECT_EQ(body["toolConfig"]["toolChoice"], (Json{{"any", Json::object()}}));
    options.toolChoice = "auto";
    EXPECT_EQ(build(model(), context({user("q")}, {tool("read")}), options)["toolConfig"]["toolChoice"],
              (Json{{"auto", Json::object()}}));
    options.toolChoice = "none";
    EXPECT_FALSE(build(model(), context({user("q")}, {tool("read")}), options).contains("toolConfig"));
}

TEST_F(BedrockRequestBuilderTest, ThinkingBlocksDependOnTheModelFamily) {
    const Model claude = model();
    ThinkingContent signed_;
    signed_.thinking = "hmm";
    signed_.thinkingSignature = "SIG";
    ThinkingContent unsigned_;
    unsigned_.thinking = "plain";
    ThinkingContent redacted;
    redacted.thinking = "[Reasoning redacted]";
    redacted.redacted = true;
    redacted.thinkingSignature = "QUJD";
    ThinkingContent brokenRedacted;
    brokenRedacted.redacted = true;
    brokenRedacted.thinkingSignature = "!!";
    StreamOptions options;
    options.cacheRetention = "none";
    const Json body = build(claude, context({user("q"), assistant(claude, {signed_, unsigned_, redacted, brokenRedacted, text("a")})}), options);
    const Json& content = body["messages"][1]["content"];
    ASSERT_EQ(content.size(), 4U);
    EXPECT_EQ(content[0], (Json{{"reasoningContent", Json{{"reasoningText", Json{{"text", "hmm"}, {"signature", "SIG"}}}}}}));
    EXPECT_EQ(content[1], (Json{{"text", "plain"}}));
    EXPECT_EQ(content[2], (Json{{"reasoningContent", Json{{"redactedContent", "QUJD"}}}}));
    EXPECT_EQ(content[3], (Json{{"text", "a"}}));

    Model other = model("openai.gpt-oss-120b-1:0");
    other.name = "GPT OSS";
    const Json plain = build(other, context({user("q"), assistant(other, {signed_})}), options);
    EXPECT_EQ(plain["messages"][1]["content"][0],
              (Json{{"reasoningContent", Json{{"reasoningText", Json{{"text", "hmm"}}}}}}));
}

TEST_F(BedrockRequestBuilderTest, BudgetBasedClaudeThinking) {
    const Model m = model("anthropic.claude-sonnet-4-20250514-v1:0", true);
    StreamOptions options;
    options.reasoning = ThinkingLevel::Medium;
    options.maxTokens = 2000;
    const Json body = build(m, context({user("q")}), options);
    EXPECT_EQ(body["additionalModelRequestFields"]["thinking"],
              (Json{{"type", "enabled"}, {"budget_tokens", 6976}, {"display", "summarized"}}));
    EXPECT_EQ(body["additionalModelRequestFields"]["anthropic_beta"], Json::array({"interleaved-thinking-2025-05-14"}));
    EXPECT_EQ(body["inferenceConfig"]["maxTokens"], 8000);
}

TEST_F(BedrockRequestBuilderTest, AdaptiveClaudeUsesEffort) {
    const Model m = model("anthropic.claude-opus-4-8-v1:0", true);
    StreamOptions options;
    options.reasoning = ThinkingLevel::XHigh;
    const Json body = build(m, context({user("q")}), options);
    EXPECT_EQ(body["additionalModelRequestFields"]["thinking"], (Json{{"type", "adaptive"}, {"display", "summarized"}}));
    EXPECT_EQ(body["additionalModelRequestFields"]["output_config"]["effort"], "xhigh");
    EXPECT_FALSE(body["additionalModelRequestFields"].contains("anthropic_beta"));
    const Json gov = build(m, context({user("q")}), options, "us-gov-west-1");
    EXPECT_EQ(gov["additionalModelRequestFields"]["thinking"], (Json{{"type", "adaptive"}}));
}

TEST_F(BedrockRequestBuilderTest, NoThinkingFieldsWithoutReasoningOrForNonClaude) {
    const Model reasoning = model("anthropic.claude-sonnet-4-20250514-v1:0", true);
    EXPECT_FALSE(build(reasoning, context({user("q")})).contains("additionalModelRequestFields"));
    Model nova = model("amazon.nova-pro-v1:0", true);
    nova.name = "Nova";
    StreamOptions options;
    options.reasoning = ThinkingLevel::High;
    EXPECT_FALSE(build(nova, context({user("q")}), options).contains("additionalModelRequestFields"));
}
