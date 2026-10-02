export module pi.types.message;

import std;
export import pi.types.assistant_message;
export import pi.types.system_message;
export import pi.types.tool_result_message;
export import pi.types.user_message;

/** One transcript entry as understood by providers. Roles: system user assistant toolResult. */
export using Message = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage>;
