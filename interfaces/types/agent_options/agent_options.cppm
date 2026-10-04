export module pi.types.agent_options;

import std;
export import pi.tool.i_tool;
export import pi.types.agent_loop_config;
export import pi.types.agent_message;
export import pi.types.model;
export import pi.types.queue_mode;
export import pi.types.thinking_level;

/**
 * Construction options of an Agent. `loopConfig` carries the hooks, stream options and tool
 * execution mode; its model and options.reasoning are overridden by `model`/`thinkingLevel`.
 * `systemPrompt` and `tools` become the leading system message unless `messages` starts with one.
 */
export struct AgentOptions {
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::optional<std::string> systemPrompt;
    std::vector<std::shared_ptr<ITool>> tools;
    std::vector<AgentMessage> messages;
    AgentLoopConfig loopConfig;
    StreamFn streamFn;
    QueueMode steeringMode = QueueMode::OneAtATime;
    QueueMode followUpMode = QueueMode::OneAtATime;
};
