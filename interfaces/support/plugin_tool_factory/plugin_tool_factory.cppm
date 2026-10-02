module;

#include <nlohmann/json.hpp>

#include "pi_plugin.h"

export module pi.support.plugin_tool_factory;

import std;
export import pi.support.plugin_tool;

/**
 * Builds a PluginTool from the definition JSON a plugin passes to register_tool:
 * {"name","description","parameters","label"?,"promptSnippet"?,"promptGuidelines"?,"executionMode"?}.
 */
export class PluginToolFactory {
public:
    Result<std::shared_ptr<ITool>> create(const std::string& definitionJson, PiToolExecuteFn execute,
                                          void* userData) const;

private:
    std::optional<std::string> stringField(const Json& object, const std::string& key) const;
};

std::optional<std::string> PluginToolFactory::stringField(const Json& object, const std::string& key) const {
    if (object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return std::nullopt;
}

Result<std::shared_ptr<ITool>> PluginToolFactory::create(const std::string& definitionJson,
                                                         PiToolExecuteFn execute, void* userData) const {
    const Json json = Json::parse(definitionJson, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected(Error{"plugin", "tool definition is not a JSON object"});
    }
    Tool tool;
    const auto name = stringField(json, "name");
    if (!name || name->empty()) {
        return std::unexpected(Error{"plugin", "tool definition needs a name"});
    }
    tool.name = *name;
    tool.description = stringField(json, "description").value_or("");
    if (json.contains("parameters")) {
        if (!json["parameters"].is_object()) {
            return std::unexpected(Error{"plugin", "tool \"" + *name + "\": parameters must be a JSON Schema object"});
        }
        tool.parameters = json["parameters"];
    }
    if (execute == nullptr) {
        return std::unexpected(Error{"plugin", "tool \"" + *name + "\" has no execute function"});
    }
    std::vector<std::string> guidelines;
    if (json.contains("promptGuidelines") && json["promptGuidelines"].is_array()) {
        for (const Json& guideline : json["promptGuidelines"]) {
            if (guideline.is_string()) {
                guidelines.push_back(guideline.get<std::string>());
            }
        }
    }
    std::optional<ToolExecutionMode> mode;
    const auto modeName = stringField(json, "executionMode");
    if (modeName == "sequential") {
        mode = ToolExecutionMode::Sequential;
    } else if (modeName == "parallel") {
        mode = ToolExecutionMode::Parallel;
    } else if (modeName) {
        return std::unexpected(Error{"plugin", "tool \"" + *name + "\": executionMode must be sequential or parallel"});
    }
    return std::make_shared<PluginTool>(std::move(tool), stringField(json, "label").value_or(*name),
                                        stringField(json, "promptSnippet").value_or(""), std::move(guidelines),
                                        mode, execute, userData);
}
