export module pi.types.agent_message;

import std;
export import pi.types.assistant_message;
export import pi.types.custom_message;
export import pi.types.system_message;
export import pi.types.tool_result_message;
export import pi.types.user_message;

/** Transcript entry in the agent: an LLM message or an application-defined custom message. */
export using AgentMessage = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage,
                                  CustomMessage>;
