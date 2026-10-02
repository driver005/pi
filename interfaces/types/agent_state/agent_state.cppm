export module pi.types.agent_state;

import std;
export import pi.tool.i_tool;
export import pi.types.agent_message;
export import pi.types.model;
export import pi.types.thinking_level;

/** Point-in-time copy of an agent's public state. */
export struct AgentState {
    /** Current prompt, replayed from the transcript's system messages. */
    std::string systemPrompt;
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::vector<std::shared_ptr<ITool>> tools;
    std::vector<AgentMessage> messages;
    bool isStreaming = false;
    /** Partial assistant message of the current streamed response. */
    std::optional<AgentMessage> streamingMessage;
    std::set<std::string> pendingToolCalls;
    /** Error of the most recent failed or aborted assistant turn. */
    std::optional<std::string> errorMessage;
};
