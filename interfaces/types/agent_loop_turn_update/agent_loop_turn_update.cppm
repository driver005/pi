export module pi.types.agent_loop_turn_update;

import std;
export import pi.types.agent_context;
export import pi.types.agent_message;
export import pi.types.model;
export import pi.types.thinking_level;

/** Replacement runtime state applied before the next provider request. */
export struct AgentLoopTurnUpdate {
    std::optional<AgentContext> context;
    /** Messages appended (with lifecycle events) before the request. */
    std::vector<AgentMessage> messages;
    std::optional<Model> model;
    std::optional<ThinkingLevel> thinkingLevel;
};
