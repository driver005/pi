module;

#include <cstdint>

export module pi.types.prompt_input;

import std;
export import pi.types.agent_snapshot;
export import pi.types.json;

/** Input to the rendering of one system prompt section, for one request preparation. */
export struct PromptInput {
    std::int64_t conversationId = 0;
    /** The request's resolution; `agent.tools` are the tools offered in this request. */
    std::shared_ptr<const AgentSnapshot> agent;
    /** Sections already in effect after replaying the active transcript. */
    std::map<std::string, std::string> shown;
};
