#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.chat_completions_stream_reader;

class ChatCompletionsStreamReaderTest : public testing::Test {
protected:
    ChatCompletionsStreamReaderTest() : m_emitter(initial()) {
        m_model.id = "gpt";
        m_model.cost.input = 1;
        m_model.cost.output = 2;
        m_model.cost.cacheRead = 0.5;
        m_reader = std::make_unique<ChatCompletionsStreamReader>(m_emitter, m_model, m_compat);
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
    ChatCompletionsCompat m_compat;
    AssistantStreamEmitter m_emitter;
    std::unique_ptr<ChatCompletionsStreamReader> m_reader;
};

TEST_F(ChatCompletionsStreamReaderTest, StreamsTextAndUsage) {
    ASSERT_TRUE(chunk(R"({"id":"c1","model":"gpt","choices":[{"delta":{"content":"Hel"}}]})"));
    ASSERT_TRUE(chunk(R"({"id":"c1","choices":[{"delta":{"content":"lo"}}]})"));
    ASSERT_TRUE(chunk(R"({"id":"c1","choices":[{"delta":{},"finish_reason":"stop"}]})"));
    ASSERT_TRUE(chunk(R"({"id":"c1","choices":[],"usage":{"prompt_tokens":1000000,"completion_tokens":500000,"prompt_tokens_details":{"cached_tokens":400000},"completion_tokens_details":{"reasoning_tokens":7}}})"));
    ASSERT_TRUE(chunk("[DONE]"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.responseId, "c1");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hello");
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.usage.input, 600000);
    EXPECT_EQ(message.usage.cacheRead, 400000);
    EXPECT_EQ(message.usage.output, 500000);
    EXPECT_EQ(message.usage.reasoning, 7);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 0.6 + 1.0 + 0.2);
    const auto all = events();
    EXPECT_EQ(all.back().type, AssistantEventType::Done);
}

TEST_F(ChatCompletionsStreamReaderTest, ToolCallsAssembledAcrossChunks) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","function":{"name":"read","arguments":"{\"pa"}}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"x\"}"}}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","function":{"name":"ls","arguments":"{}"}}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& content = m_emitter.message().content;
    ASSERT_EQ(content.size(), 2U);
    const auto& first = std::get<ToolCall>(content[0]);
    EXPECT_EQ(first.id, "call_a");
    EXPECT_EQ(first.name, "read");
    EXPECT_EQ(first.arguments["path"], "x");
    EXPECT_EQ(std::get<ToolCall>(content[1]).name, "ls");
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::ToolUse);
}

TEST_F(ChatCompletionsStreamReaderTest, ReasoningFieldsBecomeThinking) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"reasoning_content":"think "}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"reasoning_content":"more","content":"answer"}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& content = m_emitter.message().content;
    const auto& thinking = std::get<ThinkingContent>(content[0]);
    EXPECT_EQ(thinking.thinking, "think more");
    EXPECT_EQ(thinking.thinkingSignature, "reasoning_content");
    EXPECT_EQ(std::get<TextContent>(content[1]).text, "answer");
}

TEST_F(ChatCompletionsStreamReaderTest, ReasoningDetailsMergedIntoSignature) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"reasoning_details":[{"type":"reasoning.text","text":"a","index":0}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"reasoning_details":[{"type":"reasoning.text","text":"b","signature":"sig"},{"type":"reasoning.encrypted","data":"ENC","id":"x"}]}}]})"));
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& thinking = std::get<ThinkingContent>(m_emitter.message().content[0]);
    const Json details = Json::parse(*thinking.thinkingSignature);
    ASSERT_EQ(details.size(), 2U);
    EXPECT_EQ(details[0]["text"], "ab");
    EXPECT_EQ(details[0]["signature"], "sig");
    EXPECT_EQ(details[1]["data"], "ENC");
}

TEST_F(ChatCompletionsStreamReaderTest, ContentFilterIsError) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"content_filter"}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Provider finish_reason: content_filter");
}

TEST_F(ChatCompletionsStreamReaderTest, MissingFinishReasonIsError) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{"content":"x"}}]})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Stream ended without finish_reason");
}

TEST_F(ChatCompletionsStreamReaderTest, MissingFinishReasonToleratedWhenUnsupported) {
    m_compat.supportsFinishReason = false;
    ChatCompletionsStreamReader reader(m_emitter, m_model, m_compat);
    SseEvent event;
    event.data = R"({"choices":[{"delta":{"content":"x"}}]})";
    ASSERT_TRUE(reader.handle(event));
    ASSERT_TRUE(reader.finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Stop);
}

TEST_F(ChatCompletionsStreamReaderTest, ErrorChunkAbortsWithMessage) {
    const auto result = chunk(R"({"error":{"message":"rate limited","code":429}})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "rate limited");
}

TEST_F(ChatCompletionsStreamReaderTest, UsageInsideChoiceIsAccepted) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop","usage":{"prompt_tokens":10,"completion_tokens":2}}]})"));
    ASSERT_TRUE(m_reader->finish());
    EXPECT_EQ(m_emitter.message().usage.input, 10);
    EXPECT_EQ(m_emitter.message().usage.output, 2);
}

TEST_F(ChatCompletionsStreamReaderTest, CacheWriteTokensNotSubtractedFromCacheRead) {
    ASSERT_TRUE(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":1,"prompt_tokens_details":{"cached_tokens":30,"cache_write_tokens":20}}})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& usage = m_emitter.message().usage;
    EXPECT_EQ(usage.input, 50);
    EXPECT_EQ(usage.cacheRead, 30);
    EXPECT_EQ(usage.cacheWrite, 20);
    EXPECT_EQ(usage.totalTokens, 101);
}
