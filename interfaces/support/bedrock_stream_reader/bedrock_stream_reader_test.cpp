#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.base64_codec;
import pi.support.bedrock_stream_reader;

class BedrockStreamReaderTest : public testing::Test {
protected:
    BedrockStreamReaderTest() : m_emitter(initial()), m_reader(m_emitter, costed(), m_base64) {
        m_emitter.start();
    }

    Model costed() {
        Model m;
        m.id = "claude";
        m.cost.input = 1;
        m.cost.output = 2;
        return m;
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    Result<void> event(const std::string& type, const std::string& payload) {
        AwsEventStreamMessage message;
        message.headers = {{":message-type", "event"}, {":event-type", type}};
        message.payload = payload;
        return m_reader.handle(message);
    }

    Result<void> exception(const std::string& type, const std::string& payload) {
        AwsEventStreamMessage message;
        message.headers = {{":message-type", "exception"}, {":exception-type", type}};
        message.payload = payload;
        return m_reader.handle(message);
    }

    Base64Codec m_base64;
    AssistantStreamEmitter m_emitter;
    BedrockStreamReader m_reader;
};

TEST_F(BedrockStreamReaderTest, StreamsTextAndMetadata) {
    ASSERT_TRUE(event("messageStart", R"({"role":"assistant"})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"Hel"}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"lo"}})"));
    ASSERT_TRUE(event("contentBlockStop", R"({"contentBlockIndex":0})"));
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"end_turn"})"));
    ASSERT_TRUE(event("metadata", R"({"usage":{"inputTokens":1000000,"outputTokens":500000,"totalTokens":1500000,
        "cacheReadInputTokens":3,"cacheWriteInputTokens":4,"cacheDetails":[{"ttl":"1h","inputTokens":4},{"ttl":"5m","inputTokens":9}]}})"));
    ASSERT_TRUE(m_reader.finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hello");
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.rawStopReason, "end_turn");
    EXPECT_EQ(message.usage.input, 1000000);
    EXPECT_EQ(message.usage.cacheRead, 3);
    EXPECT_EQ(message.usage.cacheWrite, 4);
    EXPECT_EQ(message.usage.cacheWrite1h, 4);
    EXPECT_EQ(message.usage.totalTokens, 1500000);
    EXPECT_NEAR(message.usage.cost.total, 2.0, 1e-3);
}

TEST_F(BedrockStreamReaderTest, ToolUseBlocksAccumulateInput) {
    ASSERT_TRUE(event("contentBlockStart", R"({"contentBlockIndex":1,"start":{"toolUse":{"toolUseId":"t1","name":"read"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":1,"delta":{"toolUse":{"input":"{\"path\":"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":1,"delta":{"toolUse":{"input":"\"a\"}"}}})"));
    ASSERT_TRUE(event("contentBlockStop", R"({"contentBlockIndex":1})"));
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"tool_use"})"));
    ASSERT_TRUE(m_reader.finish());
    const auto& call = std::get<ToolCall>(m_emitter.message().content[0]);
    EXPECT_EQ(call.id, "t1");
    EXPECT_EQ(call.name, "read");
    EXPECT_EQ(call.arguments, (Json{{"path", "a"}}));
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::ToolUse);
}

TEST_F(BedrockStreamReaderTest, ReasoningTextAndSignature) {
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"text":"thi"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"text":"nk"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"signature":"AB"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"signature":"CD"}}})"));
    ASSERT_TRUE(event("contentBlockStop", R"({"contentBlockIndex":0})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":1,"delta":{"text":"done"}})"));
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"end_turn"})"));
    ASSERT_TRUE(m_reader.finish());
    const auto& thinking = std::get<ThinkingContent>(m_emitter.message().content[0]);
    EXPECT_EQ(thinking.thinking, "think");
    EXPECT_EQ(thinking.thinkingSignature, "ABCD");
    EXPECT_EQ(std::get<TextContent>(m_emitter.message().content[1]).text, "done");
}

TEST_F(BedrockStreamReaderTest, EncryptedReasoningIsKeptVerbatim) {
    // chunk bytes "abc" and "def" arrive base64 encoded separately and are joined as bytes.
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"redactedContent":"YWJj"}}})"));
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"signature":"IGNORED","redactedContent":"ZGVm"}}})"));
    ASSERT_TRUE(event("contentBlockStop", R"({"contentBlockIndex":0})"));
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"end_turn"})"));
    ASSERT_TRUE(m_reader.finish());
    const auto& thinking = std::get<ThinkingContent>(m_emitter.message().content[0]);
    EXPECT_EQ(thinking.redacted, true);
    EXPECT_EQ(thinking.thinking, "[Reasoning redacted]");
    EXPECT_EQ(thinking.thinkingSignature, m_base64.encode("abcdef"));
}

TEST_F(BedrockStreamReaderTest, UnstoppedBlocksAreClosedAtTheEnd) {
    ASSERT_TRUE(event("contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"x"}})"));
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"max_tokens"})"));
    ASSERT_TRUE(m_reader.finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Length);
}

TEST_F(BedrockStreamReaderTest, StopReasonsAndMissingStop) {
    ASSERT_TRUE(event("messageStop", R"({"stopReason":"guardrail_intervened"})"));
    const auto result = m_reader.finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Provider stopped with: guardrail_intervened");
}

TEST_F(BedrockStreamReaderTest, StreamWithoutStopReasonIsAnError) {
    const auto result = m_reader.finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Bedrock stream ended without a stop reason");
}

TEST_F(BedrockStreamReaderTest, ExceptionsUseReadablePrefixes) {
    EXPECT_EQ(exception("throttlingException", R"({"message":"slow"})").error().message, "Throttling error: slow");
    EXPECT_EQ(exception("serviceUnavailableException", R"({"message":"down"})").error().message, "Service unavailable: down");
    EXPECT_EQ(exception("validationException", R"({"message":"bad"})").error().message, "Validation error: bad");
    EXPECT_EQ(exception("internalServerException", R"({"message":"oops"})").error().message, "Internal server error: oops");
    EXPECT_EQ(exception("modelStreamErrorException", R"({"message":"m"})").error().message, "Model stream error: m");
}

TEST_F(BedrockStreamReaderTest, UserRoleStartIsRejected) {
    EXPECT_FALSE(event("messageStart", R"({"role":"user"})").has_value());
}
