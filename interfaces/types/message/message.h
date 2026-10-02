#pragma once

#include <variant>

#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/system_message/system_message.h"
#include "interfaces/types/tool_result_message/tool_result_message.h"
#include "interfaces/types/user_message/user_message.h"

/** One transcript entry as understood by providers. Roles: system user assistant toolResult. */
using Message = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage>;
