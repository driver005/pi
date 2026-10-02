#include "src/agent/agent/agent.h"

#include <gtest/gtest.h>

#include <thread>

#include "src/agent/agent_loop/agent_loop.h"
#include "src/agent/tool_call_runner/tool_call_runner.h"
#include "src/ai/faux_provider/faux_provider.h"
#include "src/testing/fixed_clock/fixed_clock.h"
#include "src/testing/inline_executor/inline_executor.h"
#include "src/testing/scripted_tool/scripted_tool.h"

class AgentTest : public testing::Test {
protected:
    AgentOptions options() {
        AgentOptions result;
        result.model.id = "faux-1";
        result.model.api = "faux";
        result.model.provider = "faux";
        result.systemPrompt = "You are helpful";
        result.streamFn = [this](const Model& model, const TranscriptContext& context,
                                 const StreamOptions& streamOptions) {
            return m_provider.stream(model, context, streamOptions);
        };
        return result;
    }

    std::unique_ptr<Agent> makeAgent(AgentOptions agentOptions) {
        return std::make_unique<Agent>(std::move(agentOptions), m_loop, m_clock);
    }

    InlineExecutor m_inline;
    FixedClock m_clock;
    FauxProvider m_provider{m_inline, m_clock};
    ToolCallRunner m_runner;
    AgentLoop m_loop{m_runner, m_inline, m_clock};
};

TEST_F(AgentTest, SystemPromptBecomesLeadingMessage) {
    auto agent = makeAgent(options());
    const AgentState state = agent->state();
    EXPECT_EQ(state.systemPrompt, "You are helpful");
    ASSERT_EQ(state.messages.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<SystemMessage>(state.messages[0]));
}

TEST_F(AgentTest, PromptRunsLoopAndRecordsTranscript) {
    m_provider.enqueue(m_provider.textResponse("hello"));
    auto agent = makeAgent(options());
    std::vector<AgentEventType> seen;
    agent->subscribe([&](const AgentEvent& event, const std::shared_ptr<AbortSignal>& signal) {
        EXPECT_NE(signal, nullptr);
        seen.push_back(event.type);
    });
    ASSERT_TRUE(agent->promptText("hi", {}).has_value());
    const AgentState state = agent->state();
    EXPECT_FALSE(state.isStreaming);
    ASSERT_EQ(state.messages.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<AssistantMessage>(state.messages[2]));
    EXPECT_EQ(seen.front(), AgentEventType::AgentStart);
    EXPECT_EQ(seen.back(), AgentEventType::AgentEnd);
}

TEST_F(AgentTest, ErrorTurnRecordsErrorMessage) {
    auto agent = makeAgent(options());
    ASSERT_TRUE(agent->promptText("hi", {}).has_value());
    EXPECT_EQ(agent->state().errorMessage, std::optional<std::string>("No more faux responses queued"));
}

TEST_F(AgentTest, PromptWhileRunningIsRejectedAndSteerGetsDelivered) {
    auto tool = std::make_shared<ScriptedTool>("slow", "done", 150);
    auto agentOptions = options();
    agentOptions.tools.push_back(tool);
    m_provider.enqueue(m_provider.toolCallResponse("slow", Json::object(), "c1"));
    m_provider.enqueue(m_provider.textResponse("finished"));
    auto agent = makeAgent(std::move(agentOptions));
    std::thread runner([&] { agent->promptText("go", {}); });
    while (!agent->state().isStreaming) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto busy = agent->promptText("again", {});
    EXPECT_FALSE(busy.has_value());
    UserMessage steer;
    steer.content = std::string("change course");
    agent->steer(steer);
    EXPECT_TRUE(agent->hasQueuedMessages());
    runner.join();
    EXPECT_FALSE(agent->hasQueuedMessages());
    const AgentState state = agent->state();
    bool sawSteer = false;
    for (const auto& message : state.messages) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            sawSteer = sawSteer || (std::holds_alternative<std::string>(user->content) &&
                                    std::get<std::string>(user->content) == "change course");
        }
    }
    EXPECT_TRUE(sawSteer);
}

TEST_F(AgentTest, AbortStopsActiveRunAndWaitForIdleReturns) {
    auto tool = std::make_shared<ScriptedTool>("slow", "done", 100);
    auto agentOptions = options();
    agentOptions.tools.push_back(tool);
    m_provider.enqueue(m_provider.toolCallResponse("slow", Json::object(), "c1"));
    m_provider.enqueue(m_provider.textResponse("never reached"));
    auto agent = makeAgent(std::move(agentOptions));
    std::thread runner([&] { agent->promptText("go", {}); });
    while (agent->state().pendingToolCalls.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    agent->abort();
    agent->waitForIdle();
    runner.join();
    EXPECT_FALSE(agent->state().isStreaming);
    EXPECT_EQ(m_provider.callCount(), 2);
}

TEST_F(AgentTest, ResetKeepsPromptBaselineAndClearsQueues) {
    m_provider.enqueue(m_provider.textResponse("hello"));
    auto agent = makeAgent(options());
    agent->promptText("hi", {});
    UserMessage queued;
    queued.content = std::string("later");
    agent->followUp(queued);
    ASSERT_TRUE(agent->reset().has_value());
    const AgentState state = agent->state();
    ASSERT_EQ(state.messages.size(), 1U);
    EXPECT_EQ(state.systemPrompt, "You are helpful");
    EXPECT_FALSE(agent->hasQueuedMessages());
    EXPECT_FALSE(state.errorMessage.has_value());
}

TEST_F(AgentTest, ContinueFromAssistantDrainsQueuedFollowUp) {
    m_provider.enqueue(m_provider.textResponse("first"));
    m_provider.enqueue(m_provider.textResponse("second"));
    auto agent = makeAgent(options());
    agent->promptText("hi", {});
    EXPECT_FALSE(agent->continueRun().has_value());
    UserMessage follow;
    follow.content = std::string("more");
    agent->followUp(follow);
    ASSERT_TRUE(agent->continueRun().has_value());
    EXPECT_EQ(agent->state().messages.size(), 5U);
}

TEST_F(AgentTest, ContinueWithoutMessagesFails) {
    auto agent = makeAgent(options());
    const auto result = agent->continueRun();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "No messages to continue from");
}

TEST_F(AgentTest, UnsubscribeStopsDelivery) {
    m_provider.enqueue(m_provider.textResponse("hello"));
    auto agent = makeAgent(options());
    int calls = 0;
    const auto id = agent->subscribe([&](const AgentEvent&, const std::shared_ptr<AbortSignal>&) { ++calls; });
    agent->unsubscribe(id);
    agent->promptText("hi", {});
    EXPECT_EQ(calls, 0);
}
