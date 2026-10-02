#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.agent.agent_loop;
import pi.agent.tool_call_runner;
import pi.ai.faux_provider;
import pi.base.thread_pool;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.scripted_tool;

class AgentLoopTest : public testing::Test {
protected:
    AgentLoopTest() {
        m_config.model.id = "faux-1";
        m_config.model.api = "faux";
        m_config.model.provider = "faux";
    }

    StreamFn streamFn() {
        return [this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_provider.stream(model, context, options);
        };
    }

    AgentMessage userPrompt(const std::string& text) {
        UserMessage message;
        message.content = text;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    std::vector<AgentMessage> runLoop(AgentLoop& loop, const std::string& prompt,
                                      const std::shared_ptr<AbortSignal>& signal = nullptr) {
        return loop.run({userPrompt(prompt)}, m_context, m_config,
                        [this](const AgentEvent& event) { m_events.push_back(event); }, signal, streamFn());
    }

    std::string roleOf(const AgentMessage& message) {
        return std::visit(
            [](const auto& value) -> std::string {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, SystemMessage>) return "system";
                if constexpr (std::is_same_v<Type, UserMessage>) return "user";
                if constexpr (std::is_same_v<Type, AssistantMessage>) return "assistant";
                if constexpr (std::is_same_v<Type, ToolResultMessage>) return "toolResult";
                return "custom";
            },
            message);
    }

    std::vector<std::string> roles(const std::vector<AgentMessage>& messages) {
        std::vector<std::string> out;
        for (const auto& message : messages) {
            out.push_back(roleOf(message));
        }
        return out;
    }

    int countEvents(AgentEventType type) {
        int count = 0;
        for (const auto& event : m_events) {
            count += event.type == type ? 1 : 0;
        }
        return count;
    }

    InlineExecutor m_inline;
    FixedClock m_clock;
    FauxProvider m_provider{m_inline, m_clock};
    ToolCallRunner m_runner;
    AgentLoop m_loop{m_runner, m_inline, m_clock};
    AgentLoopConfig m_config;
    AgentContext m_context;
    std::vector<AgentEvent> m_events;
};

TEST_F(AgentLoopTest, SingleTurnEmitsLifecycleEventsInOrder) {
    m_provider.enqueue(m_provider.textResponse("hello there"));
    const auto messages = runLoop(m_loop, "hi");
    EXPECT_EQ(roles(messages), (std::vector<std::string>{"user", "assistant"}));
    ASSERT_GE(m_events.size(), 8U);
    EXPECT_EQ(m_events.front().type, AgentEventType::AgentStart);
    EXPECT_EQ(m_events[1].type, AgentEventType::TurnStart);
    EXPECT_EQ(m_events[2].type, AgentEventType::MessageStart);
    EXPECT_EQ(m_events[3].type, AgentEventType::MessageEnd);
    EXPECT_EQ(m_events.back().type, AgentEventType::AgentEnd);
    EXPECT_EQ(m_events[m_events.size() - 2].type, AgentEventType::TurnEnd);
    EXPECT_GT(countEvents(AgentEventType::MessageUpdate), 0);
    const auto& assistant = std::get<AssistantMessage>(messages[1]);
    EXPECT_EQ(assistant.thinkingLevel, ThinkingLevel::Off);
    EXPECT_EQ(m_events.back().messages.size(), 2U);
}

TEST_F(AgentLoopTest, ToolCallRunsThenModelAnswers) {
    auto tool = std::make_shared<ScriptedTool>("lookup", "42");
    m_context.tools.push_back(tool);
    m_provider.enqueue(m_provider.toolCallResponse("lookup", Json::object(), "c1"));
    m_provider.enqueue(m_provider.textResponse("the answer is 42"));
    const auto messages = runLoop(m_loop, "what is it");
    EXPECT_EQ(roles(messages), (std::vector<std::string>{"system", "user", "assistant", "toolResult", "assistant"}));
    EXPECT_EQ(tool->executions(), 1);
    const auto& result = std::get<ToolResultMessage>(messages[3]);
    EXPECT_EQ(result.toolCallId, "c1");
    EXPECT_FALSE(result.isError);
    EXPECT_EQ(countEvents(AgentEventType::ToolExecutionStart), 1);
    EXPECT_EQ(countEvents(AgentEventType::ToolExecutionEnd), 1);
    EXPECT_EQ(countEvents(AgentEventType::TurnStart), 2);
}

TEST_F(AgentLoopTest, InitialToolsAreDeclaredInLeadingSystemMessage) {
    m_context.tools.push_back(std::make_shared<ScriptedTool>("lookup", "x"));
    m_provider.enqueue(m_provider.textResponse("ok"));
    const auto messages = runLoop(m_loop, "hi");
    const auto& system = std::get<SystemMessage>(messages[0]);
    ASSERT_TRUE(system.toolsAdded.has_value());
    EXPECT_EQ(system.toolsAdded->at(0).name, "lookup");
}

TEST_F(AgentLoopTest, UnknownToolProducesErrorResultAndLoopContinues) {
    m_provider.enqueue(m_provider.toolCallResponse("missing", Json::object(), "c1"));
    m_provider.enqueue(m_provider.textResponse("sorry"));
    const auto messages = runLoop(m_loop, "go");
    const auto& result = std::get<ToolResultMessage>(messages[2]);
    EXPECT_TRUE(result.isError);
    EXPECT_EQ(std::get<TextContent>(result.content[0]).text, "Tool missing not found");
    EXPECT_EQ(roleOf(messages.back()), "assistant");
}

TEST_F(AgentLoopTest, ParallelToolsRunConcurrentlyAndResultsKeepSourceOrder) {
    ThreadPool pool(4);
    AgentLoop loop(m_runner, pool, m_clock);
    m_context.tools.push_back(std::make_shared<ScriptedTool>("slow", "A", 200));
    m_context.tools.push_back(std::make_shared<ScriptedTool>("fast", "B", 0));
    AssistantMessage calls = m_provider.toolCallResponse("slow", Json::object(), "c1");
    ToolCall second;
    second.id = "c2";
    second.name = "fast";
    calls.content.emplace_back(second);
    m_provider.enqueue(calls);
    m_provider.enqueue(m_provider.textResponse("done"));
    const auto started = std::chrono::steady_clock::now();
    const auto messages = runLoop(loop, "go");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::milliseconds(380));
    EXPECT_EQ(std::get<ToolResultMessage>(messages[3]).toolCallId, "c1");
    EXPECT_EQ(std::get<ToolResultMessage>(messages[4]).toolCallId, "c2");
    std::vector<std::string> endOrder;
    for (const auto& event : m_events) {
        if (event.type == AgentEventType::ToolExecutionEnd) {
            endOrder.push_back(event.toolCallId);
        }
    }
    EXPECT_EQ(endOrder, (std::vector<std::string>{"c2", "c1"}));
}

TEST_F(AgentLoopTest, SequentialToolForcesOneAtATime) {
    ThreadPool pool(4);
    AgentLoop loop(m_runner, pool, m_clock);
    m_context.tools.push_back(std::make_shared<ScriptedTool>("a", "A", 100, false, true));
    m_context.tools.push_back(std::make_shared<ScriptedTool>("b", "B", 100));
    AssistantMessage calls = m_provider.toolCallResponse("a", Json::object(), "c1");
    ToolCall second;
    second.id = "c2";
    second.name = "b";
    calls.content.emplace_back(second);
    m_provider.enqueue(calls);
    m_provider.enqueue(m_provider.textResponse("done"));
    const auto started = std::chrono::steady_clock::now();
    runLoop(loop, "go");
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(190));
}

TEST_F(AgentLoopTest, TerminateHintFromAllToolsEndsRunAfterBatch) {
    m_context.tools.push_back(std::make_shared<ScriptedTool>("finish", "bye", 0, true));
    m_provider.enqueue(m_provider.toolCallResponse("finish", Json::object(), "c1"));
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(roleOf(messages.back()), "toolResult");
    EXPECT_EQ(m_provider.callCount(), 1);
    EXPECT_EQ(m_events.back().type, AgentEventType::AgentEnd);
}

TEST_F(AgentLoopTest, TruncatedToolCallsAreNotExecuted) {
    auto tool = std::make_shared<ScriptedTool>("lookup", "42");
    m_context.tools.push_back(tool);
    AssistantMessage cut = m_provider.toolCallResponse("lookup", Json::object(), "c1");
    cut.stopReason = StopReason::Length;
    m_provider.enqueue(cut);
    m_provider.enqueue(m_provider.textResponse("retrying"));
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(tool->executions(), 0);
    const auto& result = std::get<ToolResultMessage>(messages[3]);
    EXPECT_TRUE(result.isError);
    EXPECT_NE(std::get<TextContent>(result.content[0]).text.find("output token limit"), std::string::npos);
}

TEST_F(AgentLoopTest, SteeringMessageIsInjectedBeforeNextRequest) {
    m_context.tools.push_back(std::make_shared<ScriptedTool>("lookup", "42"));
    m_provider.enqueue(m_provider.toolCallResponse("lookup", Json::object(), "c1"));
    m_provider.enqueue(m_provider.textResponse("ok"));
    bool delivered = false;
    m_config.getSteeringMessages = [&]() -> std::vector<AgentMessage> {
        if (delivered || m_provider.callCount() == 0) {
            return {};
        }
        delivered = true;
        return {userPrompt("actually, steer")};
    };
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(roles(messages),
              (std::vector<std::string>{"system", "user", "assistant", "toolResult", "user", "assistant"}));
}

TEST_F(AgentLoopTest, FollowUpRunsAfterAgentWouldStop) {
    m_provider.enqueue(m_provider.textResponse("first"));
    m_provider.enqueue(m_provider.textResponse("second"));
    int polls = 0;
    m_config.getFollowUpMessages = [&]() -> std::vector<AgentMessage> {
        return ++polls == 1 ? std::vector<AgentMessage>{userPrompt("and then?")} : std::vector<AgentMessage>{};
    };
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(roles(messages), (std::vector<std::string>{"user", "assistant", "user", "assistant"}));
}

TEST_F(AgentLoopTest, AbortEndsRunWithAbortedAssistantMessage) {
    m_provider.enqueue(m_provider.textResponse("this will not finish streaming"));
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    const auto messages = runLoop(m_loop, "go", signal);
    const auto& assistant = std::get<AssistantMessage>(messages.back());
    EXPECT_EQ(assistant.stopReason, StopReason::Aborted);
    EXPECT_EQ(m_events.back().type, AgentEventType::AgentEnd);
}

TEST_F(AgentLoopTest, ProviderErrorEndsRunWithoutFurtherTurns) {
    const auto messages = runLoop(m_loop, "go");
    const auto& assistant = std::get<AssistantMessage>(messages.back());
    EXPECT_EQ(assistant.stopReason, StopReason::Error);
    EXPECT_EQ(m_provider.callCount(), 1);
}

TEST_F(AgentLoopTest, FinishTurnContinueForcesOneMoreRequest) {
    m_provider.enqueue(m_provider.textResponse("one"));
    m_provider.enqueue(m_provider.textResponse("two"));
    int decisions = 0;
    m_config.finishTurn = [&](const AgentTurnContext&, const std::shared_ptr<AbortSignal>&) {
        return ++decisions == 1 ? std::optional<AgentTurnAction>(AgentTurnAction::Continue) : std::nullopt;
    };
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(roles(messages), (std::vector<std::string>{"user", "assistant", "assistant"}));
}

TEST_F(AgentLoopTest, FinishTurnEndStopsRun) {
    m_context.tools.push_back(std::make_shared<ScriptedTool>("lookup", "42"));
    m_provider.enqueue(m_provider.toolCallResponse("lookup", Json::object(), "c1"));
    m_config.finishTurn = [](const AgentTurnContext&, const std::shared_ptr<AbortSignal>&) {
        return std::optional<AgentTurnAction>(AgentTurnAction::End);
    };
    const auto messages = runLoop(m_loop, "go");
    EXPECT_EQ(m_provider.callCount(), 1);
    EXPECT_EQ(roleOf(messages.back()), "toolResult");
}

TEST_F(AgentLoopTest, ConvertToLlmAndGetApiKeyAreApplied) {
    std::string seenKey;
    int llmCount = -1;
    m_provider.enqueue([&](const TranscriptContext& context, const StreamOptions& options, const Model&) {
        seenKey = options.apiKey.value_or("");
        llmCount = static_cast<int>(context.messages.size());
        return m_provider.textResponse("ok");
    });
    m_config.getApiKey = [](const std::string& provider) {
        return std::optional<std::string>("key-for-" + provider);
    };
    m_config.convertToLlm = [](const std::vector<AgentMessage>& messages) {
        std::vector<Message> out;
        for (const auto& message : messages) {
            if (const auto* user = std::get_if<UserMessage>(&message)) {
                out.emplace_back(*user);
            }
        }
        return out;
    };
    CustomMessage note;
    note.role = "notification";
    m_context.messages.emplace_back(note);
    runLoop(m_loop, "go");
    EXPECT_EQ(seenKey, "key-for-faux");
    EXPECT_EQ(llmCount, 1);
}

TEST_F(AgentLoopTest, RunContinueRejectsEmptyAndAssistantTail) {
    const auto empty = m_loop.runContinue(m_context, m_config, nullptr, nullptr, streamFn());
    EXPECT_FALSE(empty.has_value());
    AgentContext context;
    context.messages.emplace_back(AssistantMessage{});
    EXPECT_FALSE(m_loop.runContinue(context, m_config, nullptr, nullptr, streamFn()).has_value());
}

TEST_F(AgentLoopTest, RunContinueResumesFromUserMessage) {
    m_provider.enqueue(m_provider.textResponse("continuing"));
    m_context.messages.push_back(userPrompt("pending"));
    const auto messages = m_loop.runContinue(m_context, m_config, nullptr, nullptr, streamFn());
    ASSERT_TRUE(messages.has_value());
    EXPECT_EQ(roles(*messages), (std::vector<std::string>{"assistant"}));
}
