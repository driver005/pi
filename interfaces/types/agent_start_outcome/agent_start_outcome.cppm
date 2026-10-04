export module pi.types.agent_start_outcome;

import std;
export import pi.types.json;

/**
 * What the `before_agent_start` plugin event added: messages ({customType, content, display, details}) to send along
 * with the prompt, and a replacement for the complete system prompt of the turn.
 */
export struct AgentStartOutcome {
    std::vector<Json> messages;
    std::optional<std::string> systemPrompt;
};
