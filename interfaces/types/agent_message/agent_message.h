#pragma once

#include <variant>

#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/custom_message/custom_message.h"
#include "interfaces/types/system_message/system_message.h"
#include "interfaces/types/tool_result_message/tool_result_message.h"
#include "interfaces/types/user_message/user_message.h"

/** Transcript entry in the agent: an LLM message or an application-defined custom message. */
using AgentMessage = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage,
                                  CustomMessage>;
