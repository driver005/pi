export module pi.types.agent_turn_context;

import std;
export import pi.types.agent_context;
export import pi.types.agent_message;
export import pi.types.assistant_message;
export import pi.types.tool_result_message;

/** A completed turn: the assistant message, its tool results and the resulting context. */
export struct AgentTurnContext {
    AssistantMessage message;
    std::vector<ToolResultMessage> toolResults;
    AgentContext context;
    /** Messages this loop invocation returns if it exits at this point. */
    std::vector<AgentMessage> newMessages;
};
