#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.anthropic_stream_reader;

class AnthropicStreamReaderTest : public testing::Test {
protected:
    AnthropicStreamReaderTest() : m_emitter(initial()) {
        m_model.id = "claude";
        m_model.cost.input = 3;
        m_model.cost.output = 15;
        m_reader = std::make_unique<AnthropicStreamReader>(m_emitter, m_model, false,
                                                           std::vector<Tool>{});
        m_emitter.start();
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.api = "anthropic-messages";
        message.provider = "anthropic";
        message.model = "claude";
        message.stopReason = StopReason::Pending;
        return message;
    }

    Result<void> feed(const std::string& type, const std::string& json) {
        SseEvent event;
        event.event = type;
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
    std::unique_ptr<AnthropicStreamReader> m_reader;
};

TEST_F(AnthropicStreamReaderTest, StreamsTextAndStopReason) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"msg_1","model":"claude","usage":{"input_tokens":1000000,"output_tokens":1,"cache_read_input_tokens":0}}})"));
    ASSERT_TRUE(feed("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"lo"}})"));
    ASSERT_TRUE(feed("content_block_stop", R"({"type":"content_block_stop","index":0})"));
    ASSERT_TRUE(feed("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":100000}})"));
    ASSERT_TRUE(feed("message_stop", R"({"type":"message_stop"})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.responseId, "msg_1");
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "Hello");
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.usage.input, 1000000);
    EXPECT_EQ(message.usage.output, 100000);
    EXPECT_EQ(message.usage.totalTokens, 1100000);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 3 + 1.5);
    const auto all = events();
    ASSERT_EQ(all.size(), 6U);
    EXPECT_EQ(all.front().type, AssistantEventType::Start);
    EXPECT_EQ(all[1].type, AssistantEventType::TextStart);
    EXPECT_EQ(all[4].type, AssistantEventType::TextEnd);
    EXPECT_EQ(all[4].text, "Hello");
    EXPECT_EQ(all.back().type, AssistantEventType::Done);
}

TEST_F(AnthropicStreamReaderTest, ToolUseAccumulatesJson) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude","usage":{}}})"));
    ASSERT_TRUE(feed("content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"read","input":{}}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"a.txt\"}"}})"));
    ASSERT_TRUE(feed("content_block_stop", R"({"type":"content_block_stop","index":1})"));
    ASSERT_TRUE(feed("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{}})"));
    ASSERT_TRUE(feed("message_stop", R"({"type":"message_stop"})"));
    ASSERT_TRUE(m_reader->finish());
    const auto& call = std::get<ToolCall>(m_emitter.message().content[0]);
    EXPECT_EQ(call.id, "toolu_1");
    EXPECT_EQ(call.arguments["path"], "a.txt");
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::ToolUse);
}

TEST_F(AnthropicStreamReaderTest, ThinkingWithSignatureDelta) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude","usage":{}}})"));
    ASSERT_TRUE(feed("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hm"}})"));
    ASSERT_TRUE(feed("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}})"));
    ASSERT_TRUE(feed("content_block_stop", R"({"type":"content_block_stop","index":0})"));
    ASSERT_TRUE(feed("content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"redacted_thinking","data":"ENC"}})"));
    const auto& content = m_emitter.message().content;
    const auto& thinking = std::get<ThinkingContent>(content[0]);
    EXPECT_EQ(thinking.thinking, "hm");
    EXPECT_EQ(thinking.thinkingSignature, "sig");
    const auto& redacted = std::get<ThinkingContent>(content[1]);
    EXPECT_EQ(redacted.thinking, "[Reasoning redacted]");
    EXPECT_EQ(redacted.thinkingSignature, "ENC");
    EXPECT_EQ(redacted.redacted, true);
}

TEST_F(AnthropicStreamReaderTest, RefusalBecomesError) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude","usage":{}}})"));
    ASSERT_TRUE(feed("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"refusal","stop_details":{"explanation":"nope"}},"usage":{}})"));
    ASSERT_TRUE(feed("message_stop", R"({"type":"message_stop"})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "nope");
}

TEST_F(AnthropicStreamReaderTest, UnknownStopReasonIsError) {
    const auto result = feed("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"weird"},"usage":{}})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Unhandled stop reason: weird");
}

TEST_F(AnthropicStreamReaderTest, TruncatedStreamIsError) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude","usage":{}}})"));
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Anthropic stream ended before message_stop");
}

TEST_F(AnthropicStreamReaderTest, MissingStopReason) {
    const auto result = m_reader->finish();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Anthropic stream ended without a stop reason");
}

TEST_F(AnthropicStreamReaderTest, ErrorEventAbortsStream) {
    const auto result = feed("error", R"({"type":"error","error":{"type":"overloaded_error"}})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("overloaded_error"), std::string::npos);
}

TEST_F(AnthropicStreamReaderTest, IgnoresPingAndUnknownEvents) {
    EXPECT_TRUE(feed("ping", R"({"type":"ping"})"));
    EXPECT_TRUE(feed("message_start", "not json").has_value() == false);
}

TEST_F(AnthropicStreamReaderTest, ResponseModelDifferenceRecorded) {
    ASSERT_TRUE(feed("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude-other","usage":{}}})"));
    EXPECT_EQ(m_emitter.message().responseModel, "claude-other");
}

TEST_F(AnthropicStreamReaderTest, OAuthRestoresToolCasing) {
    Tool tool;
    tool.name = "read";
    AssistantStreamEmitter emitter(initial());
    AnthropicStreamReader reader(emitter, m_model, true, {tool});
    emitter.start();
    SseEvent event;
    event.event = "content_block_start";
    event.data = R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t","name":"Read","input":{}}})";
    ASSERT_TRUE(reader.handle(event));
    EXPECT_EQ(std::get<ToolCall>(emitter.message().content[0]).name, "read");
}
