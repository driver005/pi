#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.pi_messages_stream_reader;
import pi.testing.fixed_clock;

class PiMessagesStreamReaderTest : public testing::Test {
protected:
    PiMessagesStreamReaderTest() : m_emitter(initial()), m_clock(5000), m_reader(m_emitter, m_clock) {
        m_emitter.start();
    }

    AssistantMessage initial() {
        AssistantMessage message;
        message.stopReason = StopReason::Pending;
        return message;
    }

    void event(const std::string& json) {
        SseEvent sse;
        sse.data = json;
        ASSERT_TRUE(m_reader.handle(sse).has_value());
    }

    std::vector<AssistantMessageEvent> events() {
        std::vector<AssistantMessageEvent> out;
        auto stream = m_emitter.stream();
        while (auto next = stream->next()) {
            out.push_back(*next);
        }
        return out;
    }

    AssistantStreamEmitter m_emitter;
    FixedClock m_clock;
    PiMessagesStreamReader m_reader;
};

TEST_F(PiMessagesStreamReaderTest, ReplaysTextThinkingAndToolCalls) {
    event(R"({"type":"start"})");
    event(R"({"type":"thinking_start","contentIndex":0})");
    event(R"({"type":"thinking_delta","contentIndex":0,"delta":"hm"})");
    event(R"({"type":"thinking_end","contentIndex":0,"content":"hmm","contentSignature":"SIG","redacted":false})");
    event(R"({"type":"text_start","contentIndex":1})");
    event(R"({"type":"text_delta","contentIndex":1,"delta":"Hel"})");
    event(R"({"type":"text_end","contentIndex":1,"content":"Hello"})");
    event(R"({"type":"toolcall_start","contentIndex":2,"id":"call_1","toolName":"read"})");
    event(R"({"type":"toolcall_delta","contentIndex":2,"delta":"{\"path\":\"a\"}"})");
    event(R"({"type":"toolcall_end","contentIndex":2,"toolCall":{"type":"toolCall","id":"call_1","name":"read","arguments":{"path":"a"}}})");
    event(R"({"type":"done","reason":"toolUse","responseId":"r1","providerThinkingLevel":"high",
        "usage":{"input":10,"output":5,"cacheRead":0,"cacheWrite":0,"totalTokens":15,
                 "cost":{"input":0.1,"output":0.2,"cacheRead":0,"cacheWrite":0,"total":0.3}}})");
    ASSERT_TRUE(m_reader.finish("radius").has_value());
    const auto& message = m_emitter.message();
    ASSERT_EQ(message.content.size(), 3U);
    const auto& thinking = std::get<ThinkingContent>(message.content[0]);
    EXPECT_EQ(thinking.thinking, "hmm");
    EXPECT_EQ(thinking.thinkingSignature, "SIG");
    EXPECT_EQ(thinking.redacted, false);
    EXPECT_EQ(std::get<TextContent>(message.content[1]).text, "Hello");
    EXPECT_EQ(std::get<ToolCall>(message.content[2]).arguments, (Json{{"path", "a"}}));
    EXPECT_EQ(message.stopReason, StopReason::ToolUse);
    EXPECT_EQ(message.responseId, "r1");
    EXPECT_EQ(message.providerThinkingLevel, "high");
    EXPECT_EQ(message.usage.totalTokens, 15);
    EXPECT_DOUBLE_EQ(message.usage.cost.total, 0.3);
    EXPECT_EQ(events().back().type, AssistantEventType::Done);
}

TEST_F(PiMessagesStreamReaderTest, ErrorEventCarriesTheMessage) {
    event(R"({"type":"text_start","contentIndex":0})");
    event(R"({"type":"error","reason":"error","errorMessage":"policy blocked","usage":{"input":1,"output":0,
        "cacheRead":0,"cacheWrite":0,"totalTokens":1,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}}})");
    ASSERT_TRUE(m_reader.finish("radius").has_value());
    EXPECT_EQ(m_emitter.message().stopReason, StopReason::Error);
    EXPECT_EQ(m_emitter.message().errorMessage, "policy blocked");
    EXPECT_EQ(events().back().type, AssistantEventType::Error);
}

TEST_F(PiMessagesStreamReaderTest, RewriteImpactBecomesADiagnostic) {
    event(R"({"type":"done","reason":"stop","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,
        "cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},
        "rewrite":{"policyId":"p1","policyVersion":2,"changed":true,"tokenCountChange":-5,"messageCountChange":0,
        "systemPromptChanged":false}})");
    const Json& diagnostics = m_emitter.message().diagnostics;
    ASSERT_TRUE(diagnostics.is_array());
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0]["type"], "pi_messages_rewrite");
    EXPECT_EQ(diagnostics[0]["timestamp"], 5000);
    EXPECT_EQ(diagnostics[0]["details"]["policyId"], "p1");
}

TEST_F(PiMessagesStreamReaderTest, MissingTerminalEventIsAnError) {
    event(R"({"type":"text_start","contentIndex":0})");
    const auto result = m_reader.finish("radius");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "radius stream ended without a terminal event");
}

TEST_F(PiMessagesStreamReaderTest, MalformedEventsAndLateEvents) {
    SseEvent bad;
    bad.data = "{not json";
    EXPECT_FALSE(m_reader.handle(bad).has_value());
    event(R"({"type":"done","reason":"stop","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,
        "cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}}})");
    event(R"({"type":"text_start","contentIndex":0})");
    EXPECT_TRUE(m_emitter.message().content.empty());
}
