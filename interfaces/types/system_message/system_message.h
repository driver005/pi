#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "interfaces/types/text_content/text_content.h"
#include "interfaces/types/tool/tool.h"
#include "interfaces/types/tool_reference/tool_reference.h"

/**
 * System instructions and tool declarations at one point in the transcript. The leading one
 * is the prompt; later ones add instructions, replace named sections (nullopt removes) and
 * add or remove tools. Replaying all of them in order yields the current prompt and tools.
 */
struct SystemMessage {
    std::variant<std::string, std::vector<TextContent>> content = std::string();
    /** Ordered named sections; a nullopt value removes the section. */
    std::optional<std::vector<std::pair<std::string, std::optional<std::string>>>> sections;
    std::optional<std::vector<Tool>> toolsAdded;
    std::optional<std::vector<ToolReference>> toolsRemoved;
    std::int64_t timestamp = 0;
};
