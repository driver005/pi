#pragma once

#include "interfaces/types/agent_context/agent_context.h"
#include "interfaces/types/model/model.h"
#include "interfaces/types/thinking_level/thinking_level.h"

/** Runtime state available immediately before every provider request. */
struct PrepareRequestContext {
    const AgentContext* context = nullptr;
    const Model* model = nullptr;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
};
