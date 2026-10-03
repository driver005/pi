export module pi.mcp.read_mcp_resource_tool;

import std;
export import pi.support.mcp_resource_catalog;
export import pi.tool.i_tool;

/** The `read_mcp_resource` tool (Codex and opencode's name for it), over the resources of the connected MCP servers. */
export class ReadMcpResourceTool : public ITool {
public:
    explicit ReadMcpResourceTool(std::shared_ptr<McpResourceCatalog> catalog)
        : m_catalog(std::move(catalog)) {
        m_definition.name = "read_mcp_resource";
        m_definition.description = "Read a specific resource from an MCP server given the server name and resource URI.";
        m_definition.parameters = Json::parse(R"({"type":"object","properties":{"server":{"type":"string","description":"MCP server name exactly as configured. Must match the 'server' field returned by list_mcp_resources."},"uri":{"type":"string","description":"Resource URI to read. Must be one of the URIs returned by list_mcp_resources."}},"required":["server","uri"],"additionalProperties":false})");
    }

    const Tool& definition() const override {
        return m_definition;
    }

    std::string label() const override {
        return m_definition.name;
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

    Result<AgentToolResult> execute(const std::string&, const Json& params, const std::shared_ptr<AbortSignal>& signal, const ToolUpdateCallback&) override {
        return m_catalog->readResource(params, signal);
    }

private:
    std::shared_ptr<McpResourceCatalog> m_catalog;
    Tool m_definition;
};
