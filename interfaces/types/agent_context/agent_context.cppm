export module pi.types.agent_context;

import std;
export import pi.tool.i_tool;
export import pi.types.agent_message;

/** Transcript visible to the model plus the tools executable in this run. */
export struct AgentContext {
    std::vector<AgentMessage> messages;
    std::vector<std::shared_ptr<ITool>> tools;
};
