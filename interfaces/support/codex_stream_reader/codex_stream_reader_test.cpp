#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.codex_stream_reader;

class CodexStreamReaderTest : public testing::Test {
protected:
    CodexStreamReaderTest() : m_emitter(initial()), m_reader(m_emitter, Model{}) {
        m_emitter.start();
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    Result<void> event(const std::string& json) {
        SseEvent sse;
        sse.data = json;
        return m_reader.handle(sse);
    }

    void textMessage() {
        ASSERT_TRUE(event(R"({"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"m"}})"));
        ASSERT_TRUE(event(R"({"type":"response.output_text.delta","output_index":0,"delta":"ok"})"));
        ASSERT_TRUE(event(R"({"type":"response.output_item.done","output_index":0,
            "item":{"type":"message","id":"m","content":[{"type":"output_text","text":"ok"}]}})"));
    }

    AssistantStreamEmitter m_emitter;
    CodexStreamReader m_reader;
};

TEST_F(CodexStreamReaderTest, DoneEventCompletesTheResponseAndRecordsEndTurn) {
    textMessage();
    ASSERT_TRUE(event(R"({"type":"response.done","response":{"id":"r1","status":"completed","end_turn":true,
        "usage":{"input_tokens":4,"output_tokens":2,"total_tokens":6}}})"));
    ASSERT_TRUE(m_reader.finish());
    const auto& message = m_emitter.message();
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(message.endTurn, true);
    EXPECT_EQ(message.responseId, "r1");
    EXPECT_EQ(message.usage.totalTokens, 6);
    EXPECT_EQ(std::get<TextContent>(message.content[0]).text, "ok");
}

TEST_F(CodexStreamReaderTest, UnknownStatusIsTreatedAsMissing) {
    textMessage();
    ASSERT_TRUE(event(R"({"type":"response.completed","response":{"status":"weird"}})"));
    ASSERT_TRUE(m_reader.finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Stop);
    EXPECT_FALSE(m_emitter.message().rawStopReason.has_value());
}

TEST_F(CodexStreamReaderTest, IncompleteMaxTokensIsLength) {
    ASSERT_TRUE(event(R"({"type":"response.incomplete","response":{"status":"incomplete",
        "incomplete_details":{"reason":"max_output_tokens"}}})"));
    ASSERT_TRUE(m_reader.finish());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Length);
}

TEST_F(CodexStreamReaderTest, EventsAfterCompletionAreIgnored) {
    textMessage();
    ASSERT_TRUE(event(R"({"type":"response.completed","response":{"status":"completed"}})"));
    ASSERT_TRUE(event(R"({"type":"error","message":"late"})"));
    ASSERT_TRUE(m_reader.finish());
}

TEST_F(CodexStreamReaderTest, ErrorEventsUseCodexWording) {
    const auto flat = event(R"({"type":"error","code":"rate_limit","message":"slow down"})");
    ASSERT_FALSE(flat.has_value());
    EXPECT_EQ(flat.error().message, "Codex error: slow down");
    EXPECT_EQ(flat.error().code, "rate_limit");
    const auto nested = event(R"({"type":"error","error":{"code":"bad","message":"inner"}})");
    EXPECT_EQ(nested.error().message, "Codex error: inner");
    const auto bare = event(R"({"type":"error","code":"only_code"})");
    EXPECT_EQ(bare.error().message, "Codex error: only_code");
}

TEST_F(CodexStreamReaderTest, FailedResponseUsesTheServerMessage) {
    const auto failed = event(R"({"type":"response.failed","response":{"error":{"code":"server_error","message":"boom"}}})");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "boom");
    const auto blank = event(R"({"type":"response.failed","response":{}})");
    EXPECT_EQ(blank.error().message, "Codex response failed");
}

TEST_F(CodexStreamReaderTest, BadJsonAndMissingTerminalEvent) {
    EXPECT_FALSE(event("{bad").has_value());
    EXPECT_FALSE(m_reader.finish().has_value());
}
