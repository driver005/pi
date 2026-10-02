module;

#include <nlohmann/json.hpp>

export module pi.types.tool_info;

import std;
export import pi.types.json;

/** A registered tool as clients list it. */
export struct ToolInfo {
    std::string name;
    std::string description;
    Json parameters;
    std::string promptSnippet;
    std::vector<std::string> promptGuidelines;
    bool active = false;
};
