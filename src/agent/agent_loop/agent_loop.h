#pragma once

#include <memory>
#include <string>
#include <vector>

#include "interfaces/agent/i_agent_loop/i_agent_loop.h"
#include "interfaces/agent/i_tool_call_runner/i_tool_call_runner.h"
#include "interfaces/platform/i_clock/i_clock.h"
#include "interfaces/platform/i_executor/i_executor.h"
#include "interfaces/support/transcript_normalizer/transcript_normalizer.h"
#include "interfaces/types/agent_run_state/agent_run_state.h"
#include "interfaces/types/tool_batch_result/tool_batch_result.h"
#include "interfaces/types/tool_call_outcome/tool_call_outcome.h"

/**
 * Port of packages/agent/src/agent-loop.ts. Works with AgentMessage throughout and converts to
 * LLM messages only at the provider boundary. Tool calls of one assistant message run through
 * the injected IToolCallRunner, concurrently on the executor unless sequential mode applies.
 */
class AgentLoop : public IAgentLoop {
public:
    AgentLoop(IToolCallRunner& toolCalls, IExecutor& executor, const IClock& clock);

    std::vector<AgentMessage> run(std::vector<AgentMessage> prompts, AgentContext context,
                                  const AgentLoopConfig& config, const AgentEventSink& emit,
                                  const std::shared_ptr<AbortSignal>& signal,
                                  const StreamFn& streamFn) override;

    Result<std::vector<AgentMessage>> runContinue(AgentContext context, const AgentLoopConfig& config,
                                                  const AgentEventSink& emit,
                                                  const std::shared_ptr<AbortSignal>& signal,
                                                  const StreamFn& streamFn) override;

private:
    void emitEvent(AgentRunState& state, AgentEvent event) const;
    void emitSignal(AgentRunState& state, AgentEventType type) const;
    void emitMessagePair(AgentRunState& state, const AgentMessage& message) const;
    void emitTurnEnd(AgentRunState& state, const AssistantMessage& message,
                     const std::vector<ToolResultMessage>& toolResults) const;

    void runLoop(AgentRunState& state) const;
    bool runTurn(AgentRunState& state, std::vector<AgentMessage>& pending, bool& hasMoreToolCalls) const;
    void startTurn(AgentRunState& state, std::vector<AgentMessage>& pending,
                   std::vector<AgentMessage>& prepared) const;
    void applyUpdate(AgentRunState& state, const std::optional<AgentLoopTurnUpdate>& update) const;
    void applyRequestUpdate(AgentRunState& state, const std::optional<AgentLoopTurnUpdate>& update) const;
    void appendPending(AgentRunState& state, std::vector<AgentMessage> messages) const;
    std::vector<AgentMessage> pollSteering(const AgentRunState& state) const;

    std::vector<AgentMessage> declareToolChanges(const AgentContext& context,
                                                 std::vector<AgentMessage> pending) const;
    SystemMessage withToolChanges(const SystemMessage& message, const ToolStateChanges& changes) const;
    std::vector<Message> llmMessages(const std::vector<AgentMessage>& messages) const;

    AssistantMessage streamAssistantResponse(AgentRunState& state) const;
    AssistantMessage finalMessage(AgentRunState& state, AssistantMessageStream& stream) const;
    void handleStreamEvent(AgentRunState& state, const AssistantMessageEvent& event,
                           bool& addedPartial) const;

    ToolBatchResult executeToolCalls(AgentRunState& state, const AssistantMessage& assistant) const;
    ToolBatchResult failTruncatedToolCalls(AgentRunState& state,
                                           const std::vector<ToolCall>& calls) const;
    ToolBatchResult executeSequential(AgentRunState& state, const AssistantMessage& assistant,
                                      const std::vector<ToolCall>& calls) const;
    ToolBatchResult executeParallel(AgentRunState& state, const AssistantMessage& assistant,
                                    const std::vector<ToolCall>& calls) const;
    void emitToolStart(AgentRunState& state, const ToolCall& call) const;
    void emitToolEnd(AgentRunState& state, const ToolCallOutcome& outcome) const;
    ToolUpdateCallback updateEmitter(AgentRunState& state, const ToolCall& call) const;
    ToolResultMessage toResultMessage(const ToolCallOutcome& outcome) const;
    ToolCallOutcome errorOutcome(const ToolCall& call, const std::string& message) const;
    bool shouldTerminate(const std::vector<ToolCallOutcome>& outcomes) const;
    std::vector<ToolCall> toolCallsOf(const AssistantMessage& message) const;
    bool wantsSequential(const AgentRunState& state, const std::vector<ToolCall>& calls) const;
    AssistantMessage syntheticError(const AgentRunState& state, const std::string& text) const;

    IToolCallRunner& m_toolCalls;
    IExecutor& m_executor;
    const IClock& m_clock;
    TranscriptNormalizer m_transcript;
};
