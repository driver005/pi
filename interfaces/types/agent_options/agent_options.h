#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_loop_config/agent_loop_config.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/queue_mode/queue_mode.h"
#include "interfaces/types/thinking_level/thinking_level.h"

/**
 * Construction options of an Agent. `loopConfig` carries the hooks, stream options and tool
 * execution mode; its model and options.reasoning are overridden by `model`/`thinkingLevel`.
 * `systemPrompt` and `tools` become the leading system message unless `messages` starts with one.
 */
struct AgentOptions {
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::optional<std::string> systemPrompt;
    std::vector<std::shared_ptr<ITool>> tools;
    std::vector<AgentMessage> messages;
    AgentLoopConfig loopConfig;
    StreamFn streamFn;
    QueueMode steeringMode = QueueMode::OneAtATime;
    QueueMode followUpMode = QueueMode::OneAtATime;
};
