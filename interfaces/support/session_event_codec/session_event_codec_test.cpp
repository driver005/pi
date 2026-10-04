#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_event_codec;

class SessionEventCodecTest : public testing::Test {
protected:
    AgentSessionEvent agent(AgentEvent event, SessionEventType type = SessionEventType::Agent) {
        AgentSessionEvent out;
        out.type = type;
        out.agent = std::make_shared<const AgentEvent>(std::move(event));
        return out;
    }

    std::shared_ptr<const AgentMessage> user(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = 5;
        return std::make_shared<const AgentMessage>(message);
    }

    SessionEventCodec m_codec;
};

TEST_F(SessionEventCodecTest, LifecycleAndMessageEvents) {
    AgentEvent start;
    start.type = AgentEventType::AgentStart;
    EXPECT_EQ(m_codec.toJson(agent(start)), (Json{{"type", "agent_start"}}));
    AgentEvent messageStart;
    messageStart.type = AgentEventType::MessageStart;
    messageStart.message = user("hi");
    const Json json = m_codec.toJson(agent(messageStart));
    EXPECT_EQ(json["type"], "message_start");
    EXPECT_EQ(json["message"]["role"], "user");
    EXPECT_EQ(json["message"]["content"], "hi");
}

TEST_F(SessionEventCodecTest, AgentEndCarriesWillRetry) {
    AgentEvent end;
    end.type = AgentEventType::AgentEnd;
    end.messages.push_back(*user("x"));
    AgentSessionEvent event = agent(end, SessionEventType::AgentEnd);
    event.willRetry = true;
    const Json json = m_codec.toJson(event);
    EXPECT_EQ(json["type"], "agent_end");
    EXPECT_EQ(json["willRetry"], true);
    EXPECT_EQ(json["messages"].size(), 1U);
}

TEST_F(SessionEventCodecTest, MessageUpdateDropsPartialAndKeepsUsage) {
    auto partial = std::make_shared<AssistantMessage>();
    partial->usage.output = 7;
    ToolCall call;
    call.id = "call_1";
    call.name = "read";
    partial->content.push_back(call);
    AgentEvent update;
    update.type = AgentEventType::MessageUpdate;
    update.message = std::make_shared<const AgentMessage>(*partial);
    auto assistantEvent = std::make_shared<AssistantMessageEvent>();
    assistantEvent->type = AssistantEventType::ToolCallStart;
    assistantEvent->contentIndex = 0;
    assistantEvent->partial = partial;
    update.assistantEvent = assistantEvent;
    const Json json = m_codec.toJson(agent(update));
    EXPECT_EQ(json["type"], "message_update");
    EXPECT_EQ(json["usage"]["output"], 7);
    const Json& wire = json["assistantMessageEvent"];
    EXPECT_EQ(wire["type"], "toolcall_start");
    EXPECT_EQ(wire["id"], "call_1");
    EXPECT_EQ(wire["toolName"], "read");
    EXPECT_FALSE(wire.contains("partial"));
}

TEST_F(SessionEventCodecTest, TextDeltaAndDoneShapes) {
    AgentEvent update;
    update.type = AgentEventType::MessageUpdate;
    auto assistantEvent = std::make_shared<AssistantMessageEvent>();
    assistantEvent->type = AssistantEventType::TextDelta;
    assistantEvent->contentIndex = 2;
    assistantEvent->text = "hel";
    update.assistantEvent = assistantEvent;
    const Json delta = m_codec.toJson(agent(update))["assistantMessageEvent"];
    EXPECT_EQ(delta, (Json{{"type", "text_delta"}, {"contentIndex", 2}, {"delta", "hel"}}));
    auto done = std::make_shared<AssistantMessageEvent>();
    done->type = AssistantEventType::Done;
    done->reason = StopReason::ToolUse;
    done->message = std::make_shared<AssistantMessage>();
    update.assistantEvent = done;
    const Json wire = m_codec.toJson(agent(update))["assistantMessageEvent"];
    EXPECT_EQ(wire["type"], "done");
    EXPECT_EQ(wire["reason"], "toolUse");
    EXPECT_EQ(wire["message"]["role"], "assistant");
}

TEST_F(SessionEventCodecTest, ToolExecutionEvents) {
    AgentEvent start;
    start.type = AgentEventType::ToolExecutionStart;
    start.toolCallId = "c1";
    start.toolName = "read";
    start.args = Json{{"path", "a"}};
    EXPECT_EQ(m_codec.toJson(agent(start)),
              (Json{{"type", "tool_execution_start"}, {"toolCallId", "c1"}, {"toolName", "read"}, {"args", Json{{"path", "a"}}}}));
    AgentEvent end;
    end.type = AgentEventType::ToolExecutionEnd;
    end.toolCallId = "c1";
    end.toolName = "read";
    auto result = std::make_shared<AgentToolResult>();
    result->content.push_back(TextContent{"data", std::nullopt});
    end.result = result;
    end.isError = true;
    const Json json = m_codec.toJson(agent(end));
    EXPECT_EQ(json["type"], "tool_execution_end");
    EXPECT_EQ(json["result"]["content"][0]["text"], "data");
    EXPECT_EQ(json["isError"], true);
}

TEST_F(SessionEventCodecTest, SessionEvents) {
    AgentSessionEvent queue;
    queue.type = SessionEventType::QueueUpdate;
    queue.steering = {"a"};
    EXPECT_EQ(m_codec.toJson(queue), (Json{{"type", "queue_update"}, {"steering", {"a"}}, {"followUp", Json::array()}}));

    AgentSessionEvent compaction;
    compaction.type = SessionEventType::CompactionEnd;
    compaction.reason = "overflow";
    compaction.willRetry = true;
    compaction.errorMessage = "failed";
    const Json end = m_codec.toJson(compaction);
    EXPECT_EQ(end["type"], "compaction_end");
    EXPECT_EQ(end["aborted"], false);
    EXPECT_EQ(end["errorMessage"], "failed");
    EXPECT_FALSE(end.contains("result"));

    AgentSessionEvent retry;
    retry.type = SessionEventType::AutoRetryStart;
    retry.attempt = 1;
    retry.maxAttempts = 3;
    retry.delayMs = 2000;
    retry.errorMessage = "overloaded";
    EXPECT_EQ(m_codec.toJson(retry)["delayMs"], 2000);

    AgentSessionEvent summarization;
    summarization.type = SessionEventType::SummarizationRetryAttemptStart;
    summarization.source = "compaction";
    summarization.reason = "manual";
    EXPECT_EQ(m_codec.toJson(summarization)["reason"], "manual");
    summarization.source = "branchSummary";
    EXPECT_FALSE(m_codec.toJson(summarization).contains("reason"));

    AgentSessionEvent name;
    name.type = SessionEventType::SessionInfoChanged;
    EXPECT_TRUE(m_codec.toJson(name)["name"].is_null());

    AgentSessionEvent bash;
    bash.type = SessionEventType::BashExecutionUpdate;
    bash.delta = "out";
    EXPECT_FALSE(m_codec.toJson(bash).contains("id"));
    bash.id = "r1";
    EXPECT_EQ(m_codec.toJson(bash)["id"], "r1");
}
