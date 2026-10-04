#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.message_codec;

class MessageCodecTest : public testing::Test {
protected:
    Json roundTrip(const Json& input) {
        auto message = m_codec.messageFromJson(input);
        EXPECT_TRUE(message.has_value()) << (message ? "" : message.error().message);
        return message ? m_codec.toJson(*message) : Json();
    }

    MessageCodec m_codec;
};

TEST_F(MessageCodecTest, UserTextAndBlocksRoundTrip) {
    const Json text = Json::parse(R"({"role":"user","content":"hello","timestamp":1})");
    EXPECT_EQ(roundTrip(text), text);
    const Json blocks = Json::parse(R"({"role":"user","content":[{"type":"text","text":"a"},
        {"type":"image","data":"QQ==","mimeType":"image/png"}],"timestamp":2})");
    EXPECT_EQ(roundTrip(blocks), blocks);
}

TEST_F(MessageCodecTest, AssistantRoundTripKeepsFieldsAndOrder) {
    const Json input = Json::parse(R"({"role":"assistant","content":[
        {"type":"thinking","thinking":"hmm","thinkingSignature":"sig"},
        {"type":"text","text":"hi","textSignature":"t"},
        {"type":"toolCall","id":"c1","name":"read","arguments":{"path":"a.txt"},"thoughtSignature":"g"}],
        "api":"anthropic-messages","provider":"anthropic","model":"claude","responseId":"r1",
        "thinkingLevel":"high","usage":{"input":10,"output":5,"cacheRead":1,"cacheWrite":2,"reasoning":3,
        "totalTokens":18,"cost":{"input":0.1,"output":0.2,"cacheRead":0,"cacheWrite":0,"total":0.3}},
        "stopReason":"toolUse","endTurn":false,"timestamp":99})");
    EXPECT_EQ(roundTrip(input), input);
}

TEST_F(MessageCodecTest, ToolResultRoundTrip) {
    const Json input = Json::parse(R"({"role":"toolResult","toolCallId":"c1","toolName":"read",
        "content":[{"type":"text","text":"ok"}],"details":{"lines":3},
        "nestedCalls":{"calls":[{"id":"n1","name":"ls","status":"ok","durationMs":4}],"complete":true},
        "isError":false,"timestamp":5})");
    EXPECT_EQ(roundTrip(input), input);
}

TEST_F(MessageCodecTest, SystemMessageWithSectionsAndTools) {
    const Json input = Json::parse(R"({"role":"system","content":"base",
        "sections":{"rules":"be brief","old":null},
        "toolsAdded":[{"name":"read","description":"Read a file","parameters":{"type":"object"}}],
        "toolsRemoved":[{"name":"write"}],"timestamp":7})");
    EXPECT_EQ(roundTrip(input), input);
}

TEST_F(MessageCodecTest, UnknownRoleAndBadBlocksAreErrors) {
    EXPECT_FALSE(m_codec.messageFromJson(Json::parse(R"({"role":"alien"})")).has_value());
    EXPECT_FALSE(m_codec.messageFromJson(Json::parse(R"({"role":"user","content":[{"type":"video"}]})")).has_value());
    EXPECT_FALSE(m_codec.messageFromJson(Json::parse(R"({"content":"x"})")).has_value());
}

TEST_F(MessageCodecTest, ErrorAssistantMessageKeepsErrorFields) {
    const Json input = Json::parse(R"({"role":"assistant","content":[],"api":"a","provider":"p","model":"m",
        "usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,
        "cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},
        "stopReason":"error","errorMessage":"boom","rawStopReason":"x","timestamp":1})");
    EXPECT_EQ(roundTrip(input), input);
}

TEST_F(MessageCodecTest, EnumNamesRoundTrip) {
    for (const StopReason reason : {StopReason::Pending, StopReason::ToolUse, StopReason::Deferred}) {
        EXPECT_EQ(m_codec.parseStopReason(m_codec.stopReasonName(reason)), reason);
    }
    EXPECT_EQ(m_codec.parseThinkingLevel("xhigh"), ThinkingLevel::XHigh);
    EXPECT_FALSE(m_codec.parseThinkingLevel("bogus").has_value());
}

TEST_F(MessageCodecTest, MessageListRoundTrip) {
    const Json input = Json::parse(R"([{"role":"user","content":"a","timestamp":1},
        {"role":"user","content":"b","timestamp":2}])");
    const auto messages = m_codec.messagesFromJson(input);
    ASSERT_TRUE(messages.has_value());
    EXPECT_EQ(m_codec.toJson(*messages), input);
}
