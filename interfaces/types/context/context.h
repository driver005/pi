#pragma once

#include <optional>
#include <string>
#include <vector>

#include "interfaces/types/message/message.h"
#include "interfaces/types/tool/tool.h"

/** Request input of the public entry points; systemPrompt/tools fold into a system message. */
struct Context {
    std::optional<std::string> systemPrompt;
    std::vector<Message> messages;
    std::optional<std::vector<Tool>> tools;
};
