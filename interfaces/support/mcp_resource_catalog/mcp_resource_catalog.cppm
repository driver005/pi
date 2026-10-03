export module pi.support.mcp_resource_catalog;

import std;
export import pi.mcp.i_mcp_resource_server;
export import pi.support.abort_signal;
export import pi.support.mcp_result_converter;
export import pi.types.agent_tool_result;

/**
 * The logic behind the MCP resource tools `list_mcp_resources`, `list_mcp_resource_templates` and `read_mcp_resource`
 * (the tools Codex and opencode use), over the servers that offer resources at call time. Listings are JSON,
 * `{server?, resources | resourceTemplates: [{server, ...item}], nextCursor?, errors?}`: with a `server` one page is
 * listed and `cursor` continues it, without every page of every server (servers that fail are reported in `errors`).
 * MCP App resources (`ui://` URIs and `profile=mcp-app` HTML) are left out, and so are `_meta` and icons. Reads become
 * text and images for the model through McpResultConverter (binary resources go to temp files), with the payload as
 * `structuredContent`. Port of createMcpResourceToolDefinitions in extensions/mcp/resources.ts.
 */
export class McpResourceCatalog {
public:
    using Servers = std::function<std::vector<std::shared_ptr<IMcpResourceServer>>()>;

    static constexpr std::string_view kListResources = "list_mcp_resources";
    static constexpr std::string_view kListTemplates = "list_mcp_resource_templates";
    static constexpr std::string_view kReadResource = "read_mcp_resource";

    McpResourceCatalog(Servers servers, McpResultConverter& converter)
        : m_servers(std::move(servers)),
          m_converter(converter) {}

    Result<AgentToolResult> listResources(const Json& params, const std::shared_ptr<AbortSignal>& signal) {
        return list(std::string(kListResources), "resources", params, signal);
    }

    Result<AgentToolResult> listResourceTemplates(const Json& params, const std::shared_ptr<AbortSignal>& signal) {
        return list(std::string(kListTemplates), "resourceTemplates", params, signal);
    }

    Result<AgentToolResult> readResource(const Json& params, const std::shared_ptr<AbortSignal>& signal) {
        auto serverName = stringArgument(params, "server");
        auto uri = stringArgument(params, "uri");
        if (!serverName || !uri) {
            return std::unexpected(!serverName ? serverName.error() : uri.error());
        }
        if (!serverName->has_value()) {
            return std::unexpected(Error{"invalid_arguments", "server must be provided"});
        }
        if (!uri->has_value()) {
            return std::unexpected(Error{"invalid_arguments", "uri must be provided"});
        }
        auto server = find(**serverName);
        if (!server) {
            return std::unexpected(server.error());
        }
        auto read = (*server)->readResource(**uri, options(**server, signal));
        if (!read) {
            return std::unexpected(read.error());
        }
        return readResult(**server, **uri, *read);
    }

    /** MCP App user interfaces, which only hosts that render them can use. */
    bool isAppResource(const Json& item) const {
        const std::string uri = text(item, "uri").empty() ? text(item, "uriTemplate") : text(item, "uri");
        if (uri.starts_with("ui://")) {
            return true;
        }
        return std::regex_search(text(item, "mimeType"), m_appProfile);
    }

private:
    Result<AgentToolResult> list(const std::string& tool, const std::string& key, const Json& params, const std::shared_ptr<AbortSignal>& signal) {
        auto serverName = stringArgument(params, "server");
        auto cursor = stringArgument(params, "cursor");
        if (!serverName || !cursor) {
            return std::unexpected(!serverName ? serverName.error() : cursor.error());
        }
        Json payload = Json::object();
        if (serverName->has_value()) {
            auto server = find(**serverName);
            if (!server) {
                return std::unexpected(server.error());
            }
            auto page = key == "resources" ? (*server)->resourcesPage(*cursor, options(**server, signal))
                                           : (*server)->resourceTemplatesPage(*cursor, options(**server, signal));
            if (!page) {
                return std::unexpected(page.error());
            }
            payload["server"] = (*server)->serverName();
            payload[key] = visible((*server)->serverName(), page->items);
            if (page->nextCursor) {
                payload["nextCursor"] = *page->nextCursor;
            }
        } else {
            if (cursor->has_value()) {
                return std::unexpected(Error{"invalid_arguments", "cursor can only be used when a server is specified"});
            }
            payload = listAll(key, signal);
        }
        return limited(tool, serverName->value_or(""), std::move(payload));
    }

    Json listAll(const std::string& key, const std::shared_ptr<AbortSignal>& signal) {
        std::vector<std::shared_ptr<IMcpResourceServer>> servers = m_servers();
        std::ranges::sort(servers, [](const auto& left, const auto& right) { return left->serverName() < right->serverName(); });
        Json items = Json::array();
        Json errors = Json::array();
        for (const std::shared_ptr<IMcpResourceServer>& server : servers) {
            auto all = key == "resources" ? server->allResources(options(*server, signal)) : server->allResourceTemplates(options(*server, signal));
            if (!all) {
                errors.push_back(Json{{"server", server->serverName()}, {"error", all.error().message}});
                continue;
            }
            for (const Json& item : visible(server->serverName(), *all)) {
                items.push_back(item);
            }
        }
        Json payload = Json::object();
        payload[key] = std::move(items);
        if (!errors.empty()) {
            payload["errors"] = std::move(errors);
        }
        return payload;
    }

    /** The listed items without App resources, `_meta` and icons, each tagged with its server. */
    Json visible(const std::string& server, const std::vector<Json>& items) const {
        Json out = Json::array();
        for (const Json& item : items) {
            if (isAppResource(item)) {
                continue;
            }
            Json entry = Json::object();
            entry["server"] = server;
            for (const auto& field : item.items()) {
                if (field.key() != "_meta" && field.key() != "icons") {
                    entry[field.key()] = field.value();
                }
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    Result<AgentToolResult> readResult(IMcpResourceServer& server, const std::string& uri, const Json& read) {
        const Json contents = read.value("contents", Json::array());
        Json blocks = Json::array();
        Json plain = Json::array();
        for (const Json& entry : contents) {
            if (contents.size() > 1) {
                blocks.push_back(Json{{"type", "text"}, {"text", text(entry, "uri") + ":"}});
            }
            blocks.push_back(Json{{"type", "resource"}, {"resource", entry}});
            Json copy = Json::object();
            for (const auto& field : entry.items()) {
                if (field.key() != "_meta") {
                    copy[field.key()] = field.value();
                }
            }
            plain.push_back(std::move(copy));
        }
        if (blocks.empty()) {
            blocks.push_back(Json{{"type", "text"}, {"text", "Resource " + uri + " is empty."}});
        }
        McpCallResult call;
        call.content = std::move(blocks);
        AgentToolResult out = m_converter.convert(server.serverName(), std::string(kReadResource), call);
        out.structuredContent = Json{{"server", server.serverName()}, {"uri", uri}, {"contents", std::move(plain)}};
        return out;
    }

    /** The payload as JSON text for the model (bounded and spilled to a file when large) and as `structuredContent`. */
    Result<AgentToolResult> limited(const std::string& tool, const std::string& server, Json payload) {
        McpCallResult call;
        call.content = Json::array({Json{{"type", "text"}, {"text", payload.dump()}}});
        AgentToolResult out = m_converter.convert(server, tool, call);
        out.structuredContent = std::move(payload);
        return out;
    }

    Result<std::shared_ptr<IMcpResourceServer>> find(const std::string& name) const {
        const std::vector<std::shared_ptr<IMcpResourceServer>> servers = m_servers();
        std::string available;
        for (const std::shared_ptr<IMcpResourceServer>& server : servers) {
            if (server->serverName() == name) {
                return server;
            }
            available += (available.empty() ? "" : ", ") + server->serverName();
        }
        return std::unexpected(Error{"unknown_server", "MCP server \"" + name + "\" has no resources" +
                                                           (available.empty() ? "" : ". Servers with resources: " + available)});
    }

    McpRequestOptions options(const IMcpResourceServer& server, const std::shared_ptr<AbortSignal>& signal) const {
        McpRequestOptions request;
        request.signal = signal;
        request.timeoutMs = server.requestTimeoutMs();
        return request;
    }

    /** A trimmed string argument; nullopt when absent, null or blank. */
    Result<std::optional<std::string>> stringArgument(const Json& params, const std::string& key) const {
        if (!params.is_object() || !params.contains(key) || params[key].is_null()) {
            return std::optional<std::string>();
        }
        if (!params[key].is_string()) {
            return std::unexpected(Error{"invalid_arguments", key + " must be a string"});
        }
        const std::string& value = params[key].get_ref<const std::string&>();
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::optional<std::string>();
        }
        return std::optional<std::string>(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
    }

    std::string text(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
    }

    Servers m_servers;
    McpResultConverter& m_converter;
    std::regex m_appProfile{";\\s*profile\\s*=\\s*\"?mcp-app\"?", std::regex::icase};
};
