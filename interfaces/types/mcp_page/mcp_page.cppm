export module pi.types.mcp_page;

import std;
export import pi.types.json;

/** One page of a paginated MCP list: its items and the cursor of the next page, if there is one. */
export struct McpPage {
    std::vector<Json> items;
    std::optional<std::string> nextCursor;
};
