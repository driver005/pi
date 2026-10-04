export module pi.mcp.mcp_tool_adapter;

import std;
export import pi.mcp.i_mcp_tool_caller;
export import pi.support.mcp_result_converter;
export import pi.tool.i_tool;
export import pi.types.mcp_tool;

/**
 * One MCP tool as an agent tool: arguments go to the server as given, progress notifications become
 * partial results, and the result is converted for the model by McpResultConverter. Port of
 * createMcpToolDefinition in packages/coding-agent/src/extensions/mcp/tools.ts.
 */
export class McpToolAdapter : public ITool {
public:
    /** `name` is the model-facing name (see McpToolNamer); `timeoutMs` applies per call. */
    McpToolAdapter(std::string server, const McpTool& tool, std::string name, IMcpToolCaller& caller, McpResultConverter& converter, std::int64_t timeoutMs)
        : m_server(std::move(server)),
          m_toolName(tool.name),
          m_caller(caller),
          m_converter(converter),
          m_timeoutMs(timeoutMs) {
        m_definition = buildDefinition(tool);
        m_definition.name = std::move(name);
    }

    const Tool& definition() const override {
        return m_definition;
    }

    std::string label() const override {
        return m_server + "/" + m_toolName;
    }

    std::string promptSnippet() const override {
        return "";
    }

    std::vector<std::string> promptGuidelines() const override {
        return {};
    }

    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }

    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }

    Result<AgentToolResult> execute(const std::string&, const Json& params, const std::shared_ptr<AbortSignal>& signal, const ToolUpdateCallback& onUpdate) override {
        McpRequestOptions options;
        options.signal = signal;
        options.timeoutMs = m_timeoutMs;
        if (onUpdate) {
            options.onProgress = [this, onUpdate](const McpProgress& progress) {
                onUpdate(progressUpdate(progress));
            };
        }
        const auto result =
            m_caller.callTool(m_toolName, params.is_object() ? params : Json::object(), options);
        if (!result) {
            return std::unexpected(result.error());
        }
        return m_converter.convert(m_server, m_toolName, *result);
    }

private:
    Tool buildDefinition(const McpTool& tool) const {
        Tool definition;
        definition.description = describe(tool);
        definition.parameters = parameters(tool.inputSchema);
        return definition;
    }

    Json parameters(const Json& schema) const {
        Json out = schema.is_object() ? schema : Json::object();
        if (!out.contains("type")) {
            out["type"] = "object";
        }
        if (!out.contains("properties")) {
            out["properties"] = Json::object();
        }
        return out;
    }

    std::string describe(const McpTool& tool) const {
        if (tool.description && !trim(*tool.description).empty()) {
            return trim(*tool.description);
        }
        if (tool.title && !tool.title->empty()) {
            return *tool.title;
        }
        if (tool.annotations.is_object() && tool.annotations.contains("title") &&
            tool.annotations["title"].is_string() && !tool.annotations["title"].get<std::string>().empty()) {
            return tool.annotations["title"].get<std::string>();
        }
        return "MCP tool " + tool.name + " from server " + m_server;
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    AgentToolResult progressUpdate(const McpProgress& progress) const {
        std::string text;
        if (progress.message) {
            text = *progress.message;
        } else {
            text = "Progress " + std::format("{}", progress.progress);
            if (progress.total) {
                text += "/" + std::format("{}", *progress.total);
            }
        }
        TextContent block;
        block.text = text;
        AgentToolResult update;
        update.content.push_back(block);
        update.details = Json{{"server", m_server}, {"tool", m_toolName}};
        return update;
    }

    std::string m_server;
    std::string m_toolName;
    IMcpToolCaller& m_caller;
    McpResultConverter& m_converter;
    std::int64_t m_timeoutMs;
    Tool m_definition;
};

/** Input schemas must be objects; servers may omit `type`, and some providers reject missing `properties`. */
