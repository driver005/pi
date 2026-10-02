module;

#include <nlohmann/json.hpp>

export module pi.types.tool_call;

import std;
export import pi.types.json;

/** A tool invocation requested by the model. */
export struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
    /** Google: opaque signature for reusing thought context. */
    std::optional<std::string> thoughtSignature;
    /** OpenAI Responses namespace for dynamically loaded or namespaced tools. */
    std::optional<std::string> toolNamespace;
};
