export module pi.support.mcp_result_parser;

import std;
export import pi.types.json;
export import pi.types.mcp_call_result;
export import pi.types.mcp_tool;
export import pi.types.result;

/**
 * Validates and converts MCP results: initialize, one page of a paginated list, tools and
 * tools/call. Port of the validators in packages/mcp/src/client.ts, including the leniencies
 * (null or empty cursors end a list, resources without a name use their URI, tool results
 * without content blocks have an empty list).
 */
export class McpResultParser {
public:
    /** The object itself when it has the fields initialize must return. */
    Result<Json> initialize(const Json& value) const {
        const bool valid = value.is_object() && value.contains("protocolVersion") && value["protocolVersion"].is_string() &&
                           value.contains("capabilities") && value["capabilities"].is_object() &&
                           value.contains("serverInfo") && value["serverInfo"].is_object() &&
                           value["serverInfo"].contains("name") && value["serverInfo"]["name"].is_string() &&
                           value["serverInfo"].contains("version") && value["serverInfo"]["version"].is_string() &&
                           (!value.contains("instructions") || value["instructions"].is_string());
        if (!valid) {
            return std::unexpected(invalid("Invalid MCP initialize result"));
        }
        return value;
    }

    /** Items under `key`, each checked by `kind` ("tool", "resource" or "resourceTemplate"). */
    Result<std::vector<Json>> listPage(const std::string& method, const std::string& key, const std::string& kind, const Json& value, std::optional<std::string>& nextCursor) const {
        if (!value.is_object() || !value.contains(key) || !value[key].is_array()) {
            return std::unexpected(invalid("Invalid MCP " + method + " result"));
        }
        std::vector<Json> items;
        for (const auto& item : value[key]) {
            if (!validItem(kind, item)) {
                return std::unexpected(invalid("Invalid entry in MCP " + method + " result"));
            }
            Json copy = item;
            if (kind != "tool" && !copy.contains("name")) {
                copy["name"] = copy[kind == "resource" ? "uri" : "uriTemplate"];
            }
            items.push_back(std::move(copy));
        }
        nextCursor.reset();
        if (value.contains("nextCursor") && !value["nextCursor"].is_null()) {
            if (!value["nextCursor"].is_string()) {
                return std::unexpected(invalid("Invalid MCP " + method + " cursor"));
            }
            if (!value["nextCursor"].get<std::string>().empty()) {
                nextCursor = value["nextCursor"].get<std::string>();
            }
        }
        return items;
    }

    McpTool tool(const Json& item) const {
        McpTool out;
        out.name = item["name"].get<std::string>();
        out.title = stringField(item, "title");
        out.description = stringField(item, "description");
        out.inputSchema = item["inputSchema"];
        if (item.contains("outputSchema") && item["outputSchema"].is_object()) {
            out.outputSchema = item["outputSchema"];
        }
        if (item.contains("annotations") && item["annotations"].is_object()) {
            out.annotations = item["annotations"];
        }
        return out;
    }

    Result<McpCallResult> callResult(const Json& value) const {
        if (!value.is_object() || (value.contains("content") && !value["content"].is_array())) {
            return std::unexpected(invalid("Invalid MCP tools/call result"));
        }
        if (value.contains("structuredContent") && !value["structuredContent"].is_null() &&
            !value["structuredContent"].is_object()) {
            return std::unexpected(invalid("Invalid MCP tools/call structured content"));
        }
        McpCallResult out;
        if (value.contains("content")) {
            out.content = value["content"];
        }
        if (value.contains("structuredContent") && value["structuredContent"].is_object()) {
            out.structuredContent = value["structuredContent"];
        }
        out.isError = value.contains("isError") && value["isError"] == true;
        return out;
    }

    Result<Json> readResult(const Json& value) const {
        if (!value.is_object() || !value.contains("contents") || !value["contents"].is_array()) {
            return std::unexpected(invalid("Invalid MCP resources/read result"));
        }
        for (const auto& contents : value["contents"]) {
            const bool valid = contents.is_object() && contents.contains("uri") && contents["uri"].is_string() &&
                               ((contents.contains("text") && contents["text"].is_string()) ||
                                (contents.contains("blob") && contents["blob"].is_string()));
            if (!valid) {
                return std::unexpected(invalid("Invalid contents in MCP resources/read result"));
            }
        }
        return value;
    }

private:
    Error invalid(const std::string& message) const {
        return Error{"protocol", message};
    }

    bool validItem(const std::string& kind, const Json& item) const {
        if (!item.is_object()) {
            return false;
        }
        if (kind == "tool") {
            return item.contains("name") && item["name"].is_string() && item.contains("inputSchema") &&
                   item["inputSchema"].is_object();
        }
        const std::string key = kind == "resource" ? "uri" : "uriTemplate";
        return item.contains(key) && item[key].is_string() && (!item.contains("name") || item["name"].is_string());
    }

    std::optional<std::string> stringField(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return std::nullopt;
    }
};
