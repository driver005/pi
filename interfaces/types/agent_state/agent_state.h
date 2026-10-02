#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/thinking_level/thinking_level.h"

/** Point-in-time copy of an agent's public state. */
struct AgentState {
    /** Current prompt, replayed from the transcript's system messages. */
    std::string systemPrompt;
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::vector<std::shared_ptr<ITool>> tools;
    std::vector<AgentMessage> messages;
    bool isStreaming = false;
    /** Partial assistant message of the current streamed response. */
    std::optional<AgentMessage> streamingMessage;
    std::set<std::string> pendingToolCalls;
    /** Error of the most recent failed or aborted assistant turn. */
    std::optional<std::string> errorMessage;
};
