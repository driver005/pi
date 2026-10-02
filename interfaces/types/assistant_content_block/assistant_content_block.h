#pragma once

#include <variant>

#include "interfaces/types/text_content/text_content.h"
#include "interfaces/types/thinking_content/thinking_content.h"
#include "interfaces/types/tool_call/tool_call.h"

/** Content produced by the model. */
using AssistantContentBlock = std::variant<TextContent, ThinkingContent, ToolCall>;
