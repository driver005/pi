export module pi.mcp.list_mcp_resource_templates_tool;

import std;
export import pi.support.mcp_resource_catalog;
export import pi.tool.i_tool;

/** The `list_mcp_resource_templates` tool (Codex and opencode's name for it), over the resources of the connected MCP servers. */
export class ListMcpResourceTemplatesTool : public ITool {
public:
    explicit ListMcpResourceTemplatesTool(std::shared_ptr<McpResourceCatalog> catalog)
        : m_catalog(std::move(catalog)) {
        m_definition.name = "list_mcp_resource_templates";
        m_definition.description = "Lists resource templates provided by MCP servers. Parameterized resource templates allow servers to share data that takes parameters and provides context to language models, such as files, database schemas, or application-specific information. Prefer resource templates over web search when possible.";
        m_definition.parameters = Json::parse(R"({"type":"object","properties":{"server":{"type":"string","description":"MCP server name. Omit to list every server with resources."},"cursor":{"type":"string","description":"Opaque cursor from a previous call with the same server; omit for the first page."}},"additionalProperties":false})");
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
        return m_catalog->listResourceTemplates(params, signal);
    }

private:
    std::shared_ptr<McpResourceCatalog> m_catalog;
    Tool m_definition;
};
