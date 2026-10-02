#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "interfaces/provider/i_provider/i_provider.h"
#include "interfaces/types/after_tool_call_result/after_tool_call_result.h"
#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_loop_turn_update/agent_loop_turn_update.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/agent_turn_action/agent_turn_action.h"
#include "interfaces/types/agent_turn_context/agent_turn_context.h"
#include "interfaces/types/before_tool_call_result/before_tool_call_result.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/prepare_request_context/prepare_request_context.h"
#include "interfaces/types/stream_options/stream_options.h"
#include "interfaces/types/tool_call_context/tool_call_context.h"
#include "interfaces/types/tool_execution_mode/tool_execution_mode.h"
#include "interfaces/types/transcript_context/transcript_context.h"

/** Starts a provider request; never throws (failures arrive as stream error events). */
using StreamFn = std::function<std::shared_ptr<AssistantMessageStream>(
    const Model&, const TranscriptContext&, const StreamOptions&)>;

/**
 * Configuration of one agent-loop run. All hooks are optional except convertToLlm and must
 * not throw. Port of AgentLoopConfig (packages/agent/src/types.ts).
 */
struct AgentLoopConfig {
    Model model;
    /** Request options; `reasoning` is the thinking level of the run. */
    StreamOptions options;

    /** Maps agent messages to LLM messages before each request; drops UI-only entries. */
    std::function<std::vector<Message>(const std::vector<AgentMessage>&)> convertToLlm;
    std::function<std::vector<AgentMessage>(const std::vector<AgentMessage>&,
                                            const std::shared_ptr<AbortSignal>&)>
        transformContext;
    /** Resolves an API key per request (short-lived OAuth tokens). */
    std::function<std::optional<std::string>(const std::string& provider)> getApiKey;

    /** Called after a turn's tool results, before turn_end. nullopt keeps normal scheduling. */
    std::function<std::optional<AgentTurnAction>(const AgentTurnContext&,
                                                 const std::shared_ptr<AbortSignal>&)>
        finishTurn;
    /** Called immediately before every provider request; may replace context/model/level. */
    std::function<std::optional<AgentLoopTurnUpdate>(const PrepareRequestContext&,
                                                     const std::shared_ptr<AbortSignal>&)>
        prepareRequest;
    /** Called after turn_end when the loop continues, before the next turn starts. */
    std::function<std::optional<AgentLoopTurnUpdate>(const AgentTurnContext&)> prepareNextTurn;
    /** Steering messages injected mid-run, polled after each turn's tool calls. */
    std::function<std::vector<AgentMessage>()> getSteeringMessages;
    /** Follow-up messages processed when the agent would otherwise stop. */
    std::function<std::vector<AgentMessage>()> getFollowUpMessages;

    ToolExecutionMode toolExecution = ToolExecutionMode::Parallel;

    /** Runs after argument validation; may block the call. */
    std::function<std::optional<BeforeToolCallResult>(const ToolCallContext&,
                                                      const std::shared_ptr<AbortSignal>&)>
        beforeToolCall;
    /** Runs after execution, before result events; may override parts of the result. */
    std::function<std::optional<AfterToolCallResult>(const ToolCallContext&,
                                                     const std::shared_ptr<AbortSignal>&)>
        afterToolCall;
};
