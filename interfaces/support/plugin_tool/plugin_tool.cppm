module;

#include <nlohmann/json.hpp>

#include "pi_plugin.h"

export module pi.support.plugin_tool;

import std;
export import pi.tool.i_tool;
export import pi.types.json;

/**
 * A tool implemented by a plugin through the C ABI (see sdk/pi_plugin.h). Arguments go to the
 * plugin as JSON, partial results stream back through the update callback and the final result is
 * converted from the plugin's JSON. The abort handle given to the plugin is the AbortSignal of the
 * call (PluginHost's abort_requested reads it back).
 */
export class PluginTool : public ITool {
public:
    PluginTool(Tool definition, std::string label, std::string snippet, std::vector<std::string> guidelines,
               std::optional<ToolExecutionMode> mode, PiToolExecuteFn execute, void* userData);

    const Tool& definition() const override;
    std::string label() const override;
    std::string promptSnippet() const override;
    std::vector<std::string> promptGuidelines() const override;
    std::optional<ToolExecutionMode> executionMode() const override;
    Json prepareArguments(const Json& arguments) const override;
    Result<AgentToolResult> execute(const std::string& toolCallId, const Json& params,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback& onUpdate) override;

private:
    std::string takeString(PiOwnedString& owned) const;
    Result<AgentToolResult> parseResult(const std::string& text) const;
    Result<std::vector<UserContentBlock>> parseContent(const Json& content) const;
    std::optional<std::string> stringField(const Json& object, const std::string& key) const;

    Tool m_definition;
    std::string m_label;
    std::string m_snippet;
    std::vector<std::string> m_guidelines;
    std::optional<ToolExecutionMode> m_mode;
    PiToolExecuteFn m_execute;
    void* m_userData;
};

PluginTool::PluginTool(Tool definition, std::string label, std::string snippet,
                       std::vector<std::string> guidelines, std::optional<ToolExecutionMode> mode,
                       PiToolExecuteFn execute, void* userData)
    : m_definition(std::move(definition)),
      m_label(std::move(label)),
      m_snippet(std::move(snippet)),
      m_guidelines(std::move(guidelines)),
      m_mode(mode),
      m_execute(execute),
      m_userData(userData) {}

const Tool& PluginTool::definition() const {
    return m_definition;
}

std::string PluginTool::label() const {
    return m_label;
}

std::string PluginTool::promptSnippet() const {
    return m_snippet;
}

std::vector<std::string> PluginTool::promptGuidelines() const {
    return m_guidelines;
}

std::optional<ToolExecutionMode> PluginTool::executionMode() const {
    return m_mode;
}

Json PluginTool::prepareArguments(const Json& arguments) const {
    return arguments;
}

std::string PluginTool::takeString(PiOwnedString& owned) const {
    std::string text = owned.data != nullptr ? std::string(owned.data, owned.size) : std::string();
    if (owned.release != nullptr) {
        owned.release(owned.data, owned.size);
    }
    owned.data = nullptr;
    return text;
}

std::optional<std::string> PluginTool::stringField(const Json& object, const std::string& key) const {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return std::nullopt;
}

Result<std::vector<UserContentBlock>> PluginTool::parseContent(const Json& content) const {
    std::vector<UserContentBlock> blocks;
    if (!content.is_array()) {
        return std::unexpected(Error{"plugin", "plugin tool result needs a content array"});
    }
    for (const Json& block : content) {
        const auto type = stringField(block, "type");
        if (type == "text" && stringField(block, "text")) {
            TextContent text;
            text.text = *stringField(block, "text");
            blocks.push_back(text);
        } else if (type == "image" && stringField(block, "data") && stringField(block, "mimeType")) {
            ImageContent image;
            image.data = *stringField(block, "data");
            image.mimeType = *stringField(block, "mimeType");
            blocks.push_back(image);
        } else {
            return std::unexpected(Error{"plugin", "plugin tool result has an invalid content block"});
        }
    }
    return blocks;
}

Result<AgentToolResult> PluginTool::parseResult(const std::string& text) const {
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected(Error{"plugin", "plugin tool returned invalid JSON"});
    }
    if (const auto message = stringField(json, "error")) {
        return std::unexpected(Error{"plugin", *message});
    }
    AgentToolResult result;
    auto content = parseContent(json.contains("content") ? json["content"] : Json());
    if (!content) {
        return std::unexpected(content.error());
    }
    result.content = std::move(*content);
    result.details = json.value("details", Json());
    result.structuredContent = json.value("structuredContent", Json());
    result.isError = json.value("isError", false);
    result.terminate = json.value("terminate", false);
    return result;
}

Result<AgentToolResult> PluginTool::execute(const std::string& toolCallId, const Json& params,
                                            const std::shared_ptr<AbortSignal>& signal,
                                            const ToolUpdateCallback& onUpdate) {
    const std::string paramsText = params.dump(-1, ' ', false, Json::error_handler_t::replace);
    const auto forward = [](void* context, PiString partial) {
        const auto* callback = static_cast<const ToolUpdateCallback*>(context);
        const Json json = Json::parse(std::string(partial.data, partial.size), nullptr, false);
        if (!callback || !*callback || !json.is_object() || !json.contains("content")) {
            return;
        }
        AgentToolResult update;
        update.details = json.value("details", Json());
        for (const Json& block : json["content"]) {
            if (block.is_object() && block.value("type", "") == "text" && block.contains("text") &&
                block["text"].is_string()) {
                TextContent text;
                text.text = block["text"].get<std::string>();
                update.content.push_back(text);
            }
        }
        (*callback)(update);
    };
    PiOwnedString owned = m_execute(
        m_userData, PiString{toolCallId.data(), toolCallId.size()}, PiString{paramsText.data(), paramsText.size()},
        reinterpret_cast<const PiAbort*>(signal.get()), forward,
        const_cast<ToolUpdateCallback*>(&onUpdate));
    if (owned.data == nullptr) {
        takeString(owned);
        return std::unexpected(Error{"plugin", "plugin tool returned nothing"});
    }
    return parseResult(takeString(owned));
}
