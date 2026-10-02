#pragma once

#include <optional>
#include <string>

#include "interfaces/types/json/json.h"

/** A tool invocation requested by the model. */
struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
    /** Google: opaque signature for reusing thought context. */
    std::optional<std::string> thoughtSignature;
    /** OpenAI Responses namespace for dynamically loaded or namespaced tools. */
    std::optional<std::string> toolNamespace;
};
