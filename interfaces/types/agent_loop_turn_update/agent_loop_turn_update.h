#pragma once

#include <optional>
#include <vector>

#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/agent_message/agent_message.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/thinking_level/thinking_level.h"

/** Replacement runtime state applied before the next provider request. */
struct AgentLoopTurnUpdate {
    std::optional<AgentContext> context;
    /** Messages appended (with lifecycle events) before the request. */
    std::vector<AgentMessage> messages;
    std::optional<Model> model;
    std::optional<ThinkingLevel> thinkingLevel;
};
