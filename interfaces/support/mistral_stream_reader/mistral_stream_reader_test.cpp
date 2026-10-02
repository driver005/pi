#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mistral_stream_reader;

class MistralStreamReaderTest : public testing::Test {
protected:
    MistralStreamReaderTest() : m_emitter(initial()) {
        m_model.id = "mistral";
        m_model.cost.input = 1;
        m_model.cost.output = 2;
        m_model.cost.cacheRead = 0.5;
        m_reader = std::make_unique<MistralStreamReader>(m_emitter, m_model);
        m_emitter.start();
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    Result<void> chunk(const std::string& json) {
        SseEvent event;
        event.data = json;
        return m_reader->handle(event);
    }

    Model m_model;
    AssistantStreamEmitter m_emitter;
    std::unique_ptr<MistralStreamReader> m_reader;
};

TEST_F(MistralStreamReaderTest, StreamsTextAndUsage) {
    ASSERT_TRUE(chunk(R"({"id":"c1","choices":[{"delta":{"content":"Hel"}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":"lo"},"finish_reason":"stop"}],
        "usage":{"prompt_tokens":1000000,"completion_tokens":500000,"total_tokens":1500000,
        "prompt_tokens_details":{"cached_tokens":400000}}})"));
    ASSERT_TRUE(chunk("[DONE]"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.responseId, "c1");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hello");
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.usage.input, 600000);
    EXPECT_EQ(message.usage.cacheRead, 400000);
    EXPECT_EQ(message.usage.totalTokens, 1500000);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 0.6 + 1.0 + 0.2);
}

TEST_F(MistralStreamReaderTest, ContentChunksCarryThinkingAndText) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":[{"type":"thinking","thinking":[{"type":"text","text":"a"},{"text":"b"}]}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":[{"type":"text","text":"answer"}]},"finish_reason":"stop"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& content = m_emitter.message().content;
    ASSERT_EQ(content.size(), 2U);
    EXPECT_EQ(std::get<ThinkingContent>(content[0]).thinking, "ab");
    EXPECT_EQ(std::get<TextContent>(content[1]).text, "answer");
}

TEST_F(MistralStreamReaderTest, EmptyDeltasDoNotSplitBlocks) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":[{"type":"thinking","thinking":[{"text":"x"}]}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":""}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":[{"type":"thinking","thinking":[{"text":"y"}]}]},"finish_reason":"stop"}]})"));
    ASSERT_TRUE(m_reader->finish());
    ASSERT_EQ(m_emitter.message().content.size(), 1U);
    EXPECT_EQ(std::get<ThinkingContent>(m_emitter.message().content[0]).thinking, "xy");
}

TEST_F(MistralStreamReaderTest, ToolCallsAccumulateByIndex) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"id":"abc123XYZ","index":0,
        "function":{"name":"read","arguments":"{\"path\":"}}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"","arguments":"\"a\"}"}}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"id":"null","index":1,
        "function":{"name":"ls","arguments":{"dir":"."}}}]},"finish_reason":"tool_calls"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    ASSERT_EQ(message.content.size(), 2U);
    const auto& first = std::get<ToolCall>(message.content[0]);
    EXPECT_EQ(first.id, "abc123XYZ");
    EXPECT_EQ(first.name, "read");
    EXPECT_EQ(first.arguments, (Json{{"path", "a"}}));
    const auto& second = std::get<ToolCall>(message.content[1]);
    EXPECT_EQ(second.id.size(), 9U);
    EXPECT_EQ(second.arguments, (Json{{"dir", "."}}));
    EXPECT_EQ(message.stopReason, StopReason::ToolUse);
}

TEST_F(MistralStreamReaderTest, FinishReasonMapping) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":"x"},"finish_reason":"model_length"}]})"));
    ASSERT_TRUE(m_reader->finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Length);
}

TEST_F(MistralStreamReaderTest, UnknownFinishReasonIsAnError) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"weird"}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Provider stopped with: weird");
}

TEST_F(MistralStreamReaderTest, MissingFinishReasonAndInvalidEvents) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":"x"}}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Mistral stream ended without a finish reason");
    EXPECT_EQ(chunk(R"({"foo":1})").error().message, "Invalid Mistral streaming event");
    EXPECT_FALSE(chunk("{bad").has_value());
}
