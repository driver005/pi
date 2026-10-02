export module pi.types.prepare_request_context;

import std;
export import pi.types.agent_context;
export import pi.types.model;
export import pi.types.thinking_level;

/** Runtime state available immediately before every provider request. */
export struct PrepareRequestContext {
    const AgentContext* context = nullptr;
    const Model* model = nullptr;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
};
