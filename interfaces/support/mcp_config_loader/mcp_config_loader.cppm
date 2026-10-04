export module pi.support.mcp_config_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.mcp_server_config_validator;
export import pi.types.json;
export import pi.types.mcp_config_load_options;
export import pi.types.mcp_config_result;

/**
 * Reads `<agentDir>/mcp.json` and, for trusted projects, `<cwd>/.pi/mcp.json`, both in the shared
 * `mcpServers` shape. Project entries replace global ones of the same name. A project entry without
 * `command`, `url` or `type` overrides only `enabled`, `exposure` and `toolExposure` of the global
 * server, keeping the rest (including credentials a project could not set itself). Problems are
 * collected, not fatal. Port of packages/coding-agent/src/extensions/mcp/config.ts.
 */
export class McpConfigLoader {
public:
    explicit McpConfigLoader(IFileSystem& files)
        : m_files(files) {}

    McpConfigResult load(const McpConfigLoadOptions& options) {
        McpConfigResult result;
        readFile(join(options.agentDir, "mcp.json"), "global", result);
        if (options.projectTrusted) {
            const std::string project = join(join(options.cwd, ".pi"), "mcp.json");
            result.projectConfig = project;
            readFile(project, "project", result);
        }
        return result;
    }

private:
    void readFile(const std::string& path, const std::string& scope, McpConfigResult& result) {
        if (!m_files.exists(path)) {
            return;
        }
        const auto text = m_files.readFile(path);
        if (!text) {
            result.errors.push_back(path + ": " + text.error().message);
            return;
        }
        const Json parsed = Json::parse(*text, nullptr, false);
        if (parsed.is_discarded()) {
            result.errors.push_back(path + ": invalid JSON");
            return;
        }
        if (!parsed.is_object() || (parsed.contains("mcpServers") && !parsed["mcpServers"].is_object())) {
            result.errors.push_back(path + ": expected an object with an \"mcpServers\" object");
            return;
        }
        if (!parsed.contains("mcpServers")) {
            return;
        }
        for (const auto& entry : parsed["mcpServers"].items()) {
            applyEntry(path, scope, entry.key(), entry.value(), result);
        }
    }

    void applyEntry(const std::string& path, const std::string& scope, const std::string& name, const Json& value, McpConfigResult& result) {
        if (scope == "project" && isOverride(value)) {
            applyOverride(path, name, value, result);
            return;
        }
        applyDefinition(path, scope, name, value, result);
    }

    void applyOverride(const std::string& path, const std::string& name, const Json& value, McpConfigResult& result) {
        McpServerConfig* base = find(result, name);
        if (base == nullptr) {
            result.errors.push_back(path + ": server \"" + name +
                                    "\" needs \"command\" or \"url\", or a global server to override");
            return;
        }
        for (const auto& entry : value.items()) {
            if (entry.key() != "enabled" && entry.key() != "exposure" && entry.key() != "toolExposure") {
                result.errors.push_back(path + ": server \"" + name +
                                        "\": an override can only set enabled, exposure, toolExposure");
                return;
            }
        }
        Json merged = base->raw;
        for (const auto& entry : value.items()) {
            merged[entry.key()] = entry.value();
        }
        auto config = m_validator.validate(name, merged);
        if (!config) {
            result.errors.push_back(path + ": " + config.error().message);
            return;
        }
        config->source = base->source;
        config->scope = base->scope;
        config->overrideSource = path;
        *base = std::move(*config);
    }

    void applyDefinition(const std::string& path, const std::string& scope, const std::string& name, const Json& value, McpConfigResult& result) {
        auto config = m_validator.validate(name, value);
        if (!config) {
            result.errors.push_back(path + ": " + config.error().message);
            return;
        }
        // Names that differ only in `-` and `_` would share a namespace.
        for (const McpServerConfig& other : result.servers) {
            if (other.name != name && m_validator.namespaceOf(other.name) == m_validator.namespaceOf(name)) {
                result.errors.push_back(path + ": server \"" + name + "\" conflicts with \"" + other.name + "\"");
                return;
            }
        }
        if (scope == "project" && config->http && config->authProvider) {
            result.errors.push_back(path + ": server \"" + name +
                                    "\": auth is only allowed in the global mcp.json");
            return;
        }
        config->source = path;
        config->scope = scope;
        if (McpServerConfig* existing = find(result, name)) {
            *existing = std::move(*config);
        } else {
            result.servers.push_back(std::move(*config));
        }
    }

    McpServerConfig* find(McpConfigResult& result, const std::string& name) {
        for (McpServerConfig& server : result.servers) {
            if (server.name == name) {
                return &server;
            }
        }
        return nullptr;
    }

    bool isOverride(const Json& value) const {
        return value.is_object() && !value.contains("command") && !value.contains("url") &&
               !value.contains("type");
    }

    std::string join(const std::string& left, const std::string& right) const {
        return left.empty() || left.back() == '/' ? left + right : left + "/" + right;
    }

    IFileSystem& m_files;
    McpServerConfigValidator m_validator;
};
