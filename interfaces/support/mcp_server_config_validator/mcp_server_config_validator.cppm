export module pi.support.mcp_server_config_validator;

import std;
export import pi.types.json;
export import pi.types.mcp_exposure;
export import pi.types.mcp_server_config;
export import pi.types.result;

/**
 * Validates one `mcpServers` entry and answers questions about a validated one. Port of
 * packages/coding-agent/src/core/mcp-servers.ts. OAuth settings (`oauth`) are accepted as an
 * object and otherwise ignored: OAuth sign-in is not ported.
 */
export class McpServerConfigValidator {
public:
    /** On failure the Error message is the user-facing explanation, naming the server. */
    Result<McpServerConfig> validate(const std::string& name, const Json& raw) const {
        if (!validName(name)) {
            return std::unexpected(problem("invalid server name \"" + name +
                                           "\" (use letters, digits, \"_\" and \"-\")"));
        }
        if (!raw.is_object()) {
            return std::unexpected(problem("server \"" + name + "\" must be an object"));
        }
        McpServerConfig config;
        config.name = name;
        config.raw = resolveAliases(raw);
        const Json& value = config.raw;
        auto common = validateCommon(name, value, config);
        if (!common) {
            return std::unexpected(common.error());
        }
        const Json type = value.value("type", Json());
        const bool typeIsHttp = type.is_null() || type == "http" || type == "streamable-http";
        const bool typeIsStdio = type.is_null() || type == "stdio";
        if (value.contains("url") && value["url"].is_string() && typeIsHttp) {
            auto http = validateHttp(name, value, config);
            if (!http) {
                return std::unexpected(http.error());
            }
            return config;
        }
        if (value.contains("command") && value["command"].is_string() && typeIsStdio) {
            auto stdio = validateStdio(name, value, config);
            if (!stdio) {
                return std::unexpected(stdio.error());
            }
            return config;
        }
        return std::unexpected(problem("server \"" + name +
                                       "\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)"));
    }

    /** `mcp__<server>` with `-` replaced by `_`, like the tool names. */
    std::string namespaceOf(const std::string& server) const {
        std::string copy = server;
        std::replace(copy.begin(), copy.end(), '-', '_');
        return "mcp__" + copy;
    }

    /** Exposure of one tool: its toolExposure entry (exact name first, then patterns), else the server's. */
    McpExposure toolExposure(const McpServerConfig& config, const std::string& tool) const {
        for (const auto& [pattern, exposure] : config.toolExposure) {
            if (pattern == tool) {
                return exposure;
            }
        }
        for (const auto& [pattern, exposure] : config.toolExposure) {
            if (pattern.find('*') != std::string::npos && wildcardMatch(pattern, tool)) {
                return exposure;
            }
        }
        return config.exposure;
    }

    /** Parses "codemode", "deferred", "direct", "hidden" and the alias "codemode-deferred". */
    std::optional<McpExposure> parseExposure(const std::string& text) const {
        if (text == "codemode" || text == "codemode-deferred") {
            return McpExposure::Codemode;
        }
        if (text == "deferred") {
            return McpExposure::Deferred;
        }
        if (text == "direct") {
            return McpExposure::Direct;
        }
        if (text == "hidden") {
            return McpExposure::Hidden;
        }
        return std::nullopt;
    }

    std::string exposureName(McpExposure exposure) const {
        switch (exposure) {
        case McpExposure::Codemode:
            return "codemode";
        case McpExposure::Deferred:
            return "deferred";
        case McpExposure::Direct:
            return "direct";
        case McpExposure::Hidden:
            return "hidden";
        }
        return "codemode";
    }

private:
    Result<void> validateCommon(const std::string& name, const Json& value, McpServerConfig& config) const {
        if (value.contains("exposure")) {
            const auto parsed = value["exposure"].is_string()
                                    ? parseExposure(value["exposure"].get<std::string>())
                                    : std::nullopt;
            if (!parsed) {
                return std::unexpected(
                    problem("server \"" + name + "\": exposure must be one of " + exposureList()));
            }
            config.exposure = *parsed;
        }
        if (value.contains("toolExposure")) {
            auto checked = validateToolExposure(name, value["toolExposure"], config);
            if (!checked) {
                return checked;
            }
        }
        if (value.contains("enabled")) {
            if (!value["enabled"].is_boolean()) {
                return std::unexpected(problem("server \"" + name + "\": enabled must be a boolean"));
            }
            config.enabled = value["enabled"].get<bool>();
        }
        if (value.contains("description")) {
            if (!value["description"].is_string()) {
                return std::unexpected(problem("server \"" + name + "\": description must be a string"));
            }
            config.description = value["description"].get<std::string>();
        }
        if (value.contains("timeout")) {
            const Json& timeout = value["timeout"];
            if (!timeout.is_number() || !(timeout.get<double>() > 0)) {
                return std::unexpected(
                    problem("server \"" + name + "\": timeout must be a positive number of seconds"));
            }
            config.timeoutSeconds = timeout.get<double>();
        }
        if (value.value("type", Json()) == "sse") {
            return std::unexpected(problem("server \"" + name +
                                           "\": legacy SSE transport is not supported; use the streamable HTTP URL"));
        }
        return {};
    }

    Result<void> validateToolExposure(const std::string& name, const Json& value, McpServerConfig& config) const {
        if (!value.is_object()) {
            return std::unexpected(
                problem("server \"" + name + "\": toolExposure must map tool names to exposures"));
        }
        for (const auto& entry : value.items()) {
            const auto parsed = entry.value().is_string() ? parseExposure(entry.value().get<std::string>())
                                                          : std::nullopt;
            if (!parsed) {
                return std::unexpected(problem("server \"" + name + "\": toolExposure \"" + entry.key() +
                                               "\" must be one of " + exposureList()));
            }
            config.toolExposure.emplace_back(entry.key(), *parsed);
        }
        return {};
    }

    Result<void> validateHttp(const std::string& name, const Json& value, McpServerConfig& config) const {
        const std::string url = value["url"].get<std::string>();
        if (!httpUrl(url)) {
            return std::unexpected(problem("server \"" + name + "\": url must be an http or https URL"));
        }
        config.http = true;
        config.url = url;
        if (value.contains("headers")) {
            auto headers = stringMap(value["headers"]);
            if (!headers) {
                return std::unexpected(
                    problem("server \"" + name + "\": headers must map names to strings"));
            }
            config.headers = std::move(*headers);
        }
        if (value.contains("oauth") && !value["oauth"].is_object()) {
            return std::unexpected(problem("server \"" + name + "\": oauth must be an object"));
        }
        if (value.contains("auth")) {
            return validateAuth(name, value, config);
        }
        return {};
    }

    Result<void> validateAuth(const std::string& name, const Json& value, McpServerConfig& config) const {
        const Json& auth = value["auth"];
        if (!auth.is_object() || !auth.contains("provider") || !auth["provider"].is_string() ||
            auth["provider"].get<std::string>().empty()) {
            return std::unexpected(problem("server \"" + name + "\": auth.provider must be a provider name"));
        }
        const std::string url = value["url"].get<std::string>();
        if (url.rfind("https://", 0) != 0 && !loopback(hostOf(url))) {
            return std::unexpected(problem("server \"" + name +
                                           "\": auth requires an https URL, or http on localhost, 127.0.0.1, or [::1]"));
        }
        config.authProvider = auth["provider"].get<std::string>();
        return {};
    }

    Result<void> validateStdio(const std::string& name, const Json& value, McpServerConfig& config) const {
        config.command = value["command"].get<std::string>();
        if (value.contains("args")) {
            const Json& args = value["args"];
            if (!args.is_array() ||
                !std::all_of(args.begin(), args.end(), [](const Json& arg) { return arg.is_string(); })) {
                return std::unexpected(problem("server \"" + name + "\": args must be an array of strings"));
            }
            config.args = args.get<std::vector<std::string>>();
        }
        if (value.contains("env")) {
            auto env = stringMap(value["env"]);
            if (!env) {
                return std::unexpected(problem("server \"" + name + "\": env must map names to strings"));
            }
            config.env = std::move(*env);
        }
        if (value.contains("cwd")) {
            if (!value["cwd"].is_string()) {
                return std::unexpected(problem("server \"" + name + "\": cwd must be a string"));
            }
            config.cwd = value["cwd"].get<std::string>();
        }
        return {};
    }

    Result<std::map<std::string, std::string>> stringMap(const Json& value) const {
        if (!value.is_object()) {
            return std::unexpected(problem("not an object"));
        }
        std::map<std::string, std::string> out;
        for (const auto& entry : value.items()) {
            if (!entry.value().is_string()) {
                return std::unexpected(problem("not strings"));
            }
            out[entry.key()] = entry.value().get<std::string>();
        }
        return out;
    }

    Json resolveAliases(const Json& raw) const {
        Json resolved = raw;
        const auto resolve = [this](const Json& value) -> Json {
            if (value.is_string()) {
                const auto parsed = parseExposure(value.get<std::string>());
                if (parsed) {
                    return exposureName(*parsed);
                }
            }
            return value;
        };
        if (resolved.contains("exposure")) {
            resolved["exposure"] = resolve(resolved["exposure"]);
        }
        if (resolved.contains("toolExposure") && resolved["toolExposure"].is_object()) {
            for (auto& entry : resolved["toolExposure"].items()) {
                entry.value() = resolve(entry.value());
            }
        }
        return resolved;
    }

    bool validName(const std::string& name) const {
        if (name.empty()) {
            return false;
        }
        return std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '-';
        });
    }

    bool httpUrl(const std::string& url) const {
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::size_t schemeEnd = lower.rfind("http://", 0) == 0 ? 7 : (lower.rfind("https://", 0) == 0 ? 8 : 0);
        return schemeEnd != 0 && lower.size() > schemeEnd && lower[schemeEnd] != '/' &&
               lower.find(' ') == std::string::npos;
    }

    std::string hostOf(const std::string& url) const {
        std::string rest = url.substr(url.find("://") + 3);
        rest = rest.substr(0, rest.find_first_of("/?#"));
        const std::size_t at = rest.rfind('@');
        if (at != std::string::npos) {
            rest = rest.substr(at + 1);
        }
        if (!rest.empty() && rest[0] == '[') {
            return rest.substr(0, rest.find(']') + 1);
        }
        std::string host = rest.substr(0, rest.find(':'));
        std::transform(host.begin(), host.end(), host.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return host;
    }

    bool loopback(const std::string& host) const {
        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }

    bool wildcardMatch(const std::string& pattern, const std::string& text) const {
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string::npos;
        std::size_t mark = 0;
        while (t < text.size()) {
            if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                mark = t;
            } else if (p < pattern.size() && pattern[p] == text[t]) {
                ++p;
                ++t;
            } else if (star != std::string::npos) {
                p = star + 1;
                t = ++mark;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    std::string exposureList() const {
        return "\"codemode\", \"deferred\", \"direct\", \"hidden\"";
    }

    Error problem(const std::string& message) const {
        return Error{"mcp_config", message};
    }
};
