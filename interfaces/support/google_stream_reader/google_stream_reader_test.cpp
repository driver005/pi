#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.google_stream_reader;
import pi.testing.fixed_clock;

class GoogleStreamReaderTest : public testing::Test {
protected:
    GoogleStreamReaderTest() : m_emitter(initial()), m_clock(1000) {
        m_model.id = "gemini";
        m_model.cost.input = 1;
        m_model.cost.output = 2;
        m_model.cost.cacheRead = 0.5;
        m_reader = std::make_unique<GoogleStreamReader>(m_emitter, m_model, m_clock);
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

    std::vector<AssistantMessageEvent> events() {
        std::vector<AssistantMessageEvent> out;
        auto stream = m_emitter.stream();
        while (auto next = stream->next()) {
            out.push_back(*next);
        }
        return out;
    }

    Model m_model;
    AssistantStreamEmitter m_emitter;
    FixedClock m_clock;
    std::unique_ptr<GoogleStreamReader> m_reader;
};

TEST_F(GoogleStreamReaderTest, StreamsTextAndUsage) {
    ASSERT_TRUE(chunk(R"({"responseId":"r1","candidates":[{"content":{"parts":[{"text":"Hel"}]}}]})"));
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"lo"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":1000000,"cachedContentTokenCount":400000,
        "candidatesTokenCount":300000,"thoughtsTokenCount":200000,"totalTokenCount":1500000}})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.responseId, "r1");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hello");
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.usage.input, 600000);
    EXPECT_EQ(message.usage.output, 500000);
    EXPECT_EQ(message.usage.cacheRead, 400000);
    EXPECT_EQ(message.usage.reasoning, 200000);
    EXPECT_EQ(message.usage.totalTokens, 1500000);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 0.6 + 1.0 + 0.2);
    EXPECT_EQ(events().back().type, AssistantEventType::Done);
}

TEST_F(GoogleStreamReaderTest, ThoughtsAndTextAlternateAndKeepSignatures) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"think","thought":true,"thoughtSignature":"SIG1"}]}}]})"));
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":" more","thought":true}]}}]})"));
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"answer","thoughtSignature":"SIG2"}]},
        "finishReason":"STOP"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& content = m_emitter.message().content;
    ASSERT_EQ(content.size(), 2U);
    EXPECT_EQ(std::get<ThinkingContent>(content[0]).thinking, "think more");
    EXPECT_EQ(std::get<ThinkingContent>(content[0]).thinkingSignature, "SIG1");
    EXPECT_EQ(std::get<TextContent>(content[1]).text, "answer");
    EXPECT_EQ(std::get<TextContent>(content[1]).textSignature, "SIG2");
}

TEST_F(GoogleStreamReaderTest, FunctionCallsBecomeToolUse) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"calling"},
        {"functionCall":{"name":"read","args":{"path":"a"},"id":"call-1"},"thoughtSignature":"SIG"}]},
        "finishReason":"STOP"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    ASSERT_EQ(message.content.size(), 2U);
    const auto& call = std::get<ToolCall>(message.content[1]);
    EXPECT_EQ(call.id, "call-1");
    EXPECT_EQ(call.name, "read");
    EXPECT_EQ(call.arguments, (Json{{"path", "a"}}));
    EXPECT_EQ(call.thoughtSignature, "SIG");
    EXPECT_EQ(message.stopReason, StopReason::ToolUse);
}

TEST_F(GoogleStreamReaderTest, MissingOrDuplicateCallIdsGetGeneratedOnes) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"a","args":{}}},
        {"functionCall":{"name":"b","id":"x"}},{"functionCall":{"name":"c","id":"x"}}]},"finishReason":"STOP"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& content = m_emitter.message().content;
    EXPECT_EQ(std::get<ToolCall>(content[0]).id, "a_1000_1");
    EXPECT_EQ(std::get<ToolCall>(content[1]).id, "x");
    EXPECT_EQ(std::get<ToolCall>(content[2]).id, "c_1000_2");
    EXPECT_EQ(std::get<ToolCall>(content[1]).arguments, Json::object());
}

TEST_F(GoogleStreamReaderTest, FinishReasonMapping) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"MAX_TOKENS"}]})"));
    ASSERT_TRUE(m_reader->finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Length);
}

TEST_F(GoogleStreamReaderTest, SafetyStopsAreErrors) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"finishReason":"SAFETY"}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Provider stopped with: SAFETY");
}

TEST_F(GoogleStreamReaderTest, StreamWithoutFinishReasonIsAnError) {
    ASSERT_TRUE(chunk(R"({"candidates":[{"content":{"parts":[{"text":"x"}]}}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Google stream ended without a finish reason");
}

TEST_F(GoogleStreamReaderTest, ErrorChunksAndBadJson) {
    const auto error = chunk(R"({"error":{"code":429,"message":"quota"}})");
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().message, "quota");
    EXPECT_FALSE(chunk("{bad").has_value());
}
