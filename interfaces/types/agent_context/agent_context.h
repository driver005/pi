#pragma once

#include <memory>
#include <vector>

#include "interfaces/tool/i_tool/i_tool.h"
#include "interfaces/types/agent_message/agent_message.h"

/** Transcript visible to the model plus the tools executable in this run. */
struct AgentContext {
    std::vector<AgentMessage> messages;
    std::vector<std::shared_ptr<ITool>> tools;
};
