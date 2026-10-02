#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.assistant_stream_emitter;

class AssistantStreamEmitterTest : public testing::Test {
protected:
    std::vector<AssistantMessageEvent> drain(AssistantStreamEmitter& emitter) {
        std::vector<AssistantMessageEvent> events;
        while (auto event = emitter.stream()->next()) {
            events.push_back(*event);
        }
        return events;
    }
};

TEST_F(AssistantStreamEmitterTest, TextBlockLifecycleAndFinalResult) {
    AssistantStreamEmitter emitter{AssistantMessage{}};
    emitter.start();
    const int index = emitter.textStart();
    emitter.textDelta(index, "he");
    emitter.textDelta(index, "llo");
    emitter.textEnd(index);
    emitter.done(StopReason::Stop);
    const auto events = drain(emitter);
    ASSERT_EQ(events.size(), 6U);
    EXPECT_EQ(events[0].type, AssistantEventType::Start);
    EXPECT_EQ(events[3].type, AssistantEventType::TextDelta);
    EXPECT_EQ(events[4].text, "hello");
    EXPECT_EQ(std::get<TextContent>(events[2].partial->content[0]).text, "he");
    const auto result = emitter.stream()->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "hello");
    EXPECT_EQ(result->stopReason, StopReason::Stop);
}

TEST_F(AssistantStreamEmitterTest, ToolCallArgumentsStreamAndFinalize) {
    AssistantStreamEmitter emitter{AssistantMessage{}};
    emitter.start();
    const int index = emitter.toolCallStart("c1", "read");
    emitter.toolCallDelta(index, "{\"path\":\"a");
    emitter.toolCallDelta(index, ".txt\",\"limit\":5}");
    emitter.toolCallEnd(index);
    emitter.done(StopReason::ToolUse);
    const auto events = drain(emitter);
    const AssistantMessageEvent& partialEvent = events[2];
    EXPECT_EQ(std::get<ToolCall>(partialEvent.partial->content[0]).arguments["path"], "a");
    const AssistantMessageEvent& end = events[4];
    ASSERT_EQ(end.type, AssistantEventType::ToolCallEnd);
    EXPECT_EQ(end.toolCall->arguments, Json::parse(R"({"path":"a.txt","limit":5})"));
}

TEST_F(AssistantStreamEmitterTest, ErrorTerminatesWithMessage) {
    AssistantStreamEmitter emitter{AssistantMessage{}};
    emitter.error(StopReason::Aborted, "stopped");
    emitter.textStart();
    const auto events = drain(emitter);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, AssistantEventType::Error);
    const auto result = emitter.stream()->result();
    EXPECT_EQ(result->errorMessage, "stopped");
    EXPECT_EQ(result->stopReason, StopReason::Aborted);
}

TEST_F(AssistantStreamEmitterTest, ThinkingBlocksAndEmptyToolArguments) {
    AssistantStreamEmitter emitter{AssistantMessage{}};
    const int thinking = emitter.thinkingStart();
    emitter.thinkingDelta(thinking, "hmm");
    emitter.thinkingEnd(thinking);
    const int tool = emitter.toolCallStart("c", "ls");
    emitter.toolCallEnd(tool);
    emitter.done(StopReason::ToolUse);
    const auto result = emitter.stream()->result();
    EXPECT_EQ(std::get<ThinkingContent>(result->content[0]).thinking, "hmm");
    EXPECT_TRUE(std::get<ToolCall>(result->content[1]).arguments.is_object());
}
