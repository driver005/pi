export module pi.support.config_value_resolver;

import std;
export import pi.platform.i_environment;
export import pi.platform.i_process_runner;
export import pi.types.config_value_part;
export import pi.types.result;

/**
 * Resolves configuration values that may be shell commands, environment references or literals:
 * "!cmd" runs the command and uses its trimmed stdout (cached per command), "$NAME" and
 * "${NAME}" interpolate environment variables, "$$" and "$!" escape, anything else is literal.
 * Port of packages/coding-agent/src/core/resolve-config-value.ts.
 */
export class ConfigValueResolver {
public:
    using Env = std::map<std::string, std::string>;
    using Headers = std::map<std::string, std::string>;

    ConfigValueResolver(const IEnvironment& environment, IProcessRunner& processes);

    bool isCommand(const std::string& config) const;

    /** Variable name when the whole value is a single reference like "$KEY" or "${KEY}". */
    std::optional<std::string> envVarName(const std::string& config) const;
    std::vector<std::string> envVarNames(const std::string& config) const;
    std::vector<std::string> missingEnvVarNames(const std::string& config, const Env& env) const;
    bool isConfigured(const std::string& config, const Env& env) const;

    /** Command results are cached for the life of this resolver. nullopt: unresolved. */
    std::optional<std::string> resolve(const std::string& config, const Env& env);
    std::optional<std::string> resolveUncached(const std::string& config, const Env& env);
    /** Error message names the description and the failing command or variables. */
    Result<std::string> resolveOrError(const std::string& config, const std::string& description,
                                       const Env& env);

    /** Resolved headers; entries that do not resolve are dropped. */
    Headers resolveHeaders(const Headers& headers, const Env& env);
    Result<Headers> resolveHeadersOrError(const Headers& headers, const std::string& description,
                                          const Env& env);

    void clearCache();

private:
    std::vector<ConfigValuePart> parseTemplate(const std::string& config) const;
    void appendLiteral(std::vector<ConfigValuePart>& parts, const std::string& value) const;
    std::optional<std::string> resolveParts(const std::vector<ConfigValuePart>& parts,
                                            const Env& env) const;
    std::optional<std::string> envValue(const std::string& name, const Env& env) const;
    std::optional<std::string> runCommand(const std::string& commandConfig);
    bool isNameStart(char c) const;
    bool isNameChar(char c) const;
    bool isValidName(const std::string& name) const;

    const IEnvironment& m_environment;
    IProcessRunner& m_processes;
    std::mutex m_mutex;
    std::map<std::string, std::optional<std::string>> m_cache;
};

ConfigValueResolver::ConfigValueResolver(const IEnvironment& environment, IProcessRunner& processes)
    : m_environment(environment), m_processes(processes) {}

bool ConfigValueResolver::isNameStart(char c) const {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool ConfigValueResolver::isNameChar(char c) const {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool ConfigValueResolver::isValidName(const std::string& name) const {
    return !name.empty() && isNameStart(name[0]) &&
           std::all_of(name.begin(), name.end(), [this](char c) { return isNameChar(c); });
}

void ConfigValueResolver::appendLiteral(std::vector<ConfigValuePart>& parts,
                                        const std::string& value) const {
    if (value.empty()) {
        return;
    }
    if (!parts.empty() && !parts.back().isEnv) {
        parts.back().value += value;
        return;
    }
    parts.push_back(ConfigValuePart{false, value});
}

std::vector<ConfigValuePart> ConfigValueResolver::parseTemplate(const std::string& config) const {
    std::vector<ConfigValuePart> parts;
    std::size_t index = 0;
    while (index < config.size()) {
        const std::size_t dollar = config.find('$', index);
        if (dollar == std::string::npos) {
            appendLiteral(parts, config.substr(index));
            break;
        }
        appendLiteral(parts, config.substr(index, dollar - index));
        const char next = dollar + 1 < config.size() ? config[dollar + 1] : '\0';
        if (next == '$' || next == '!') {
            appendLiteral(parts, std::string(1, next));
            index = dollar + 2;
        } else if (next == '{') {
            const std::size_t end = config.find('}', dollar + 2);
            if (end == std::string::npos) {
                appendLiteral(parts, "$");
                index = dollar + 1;
                continue;
            }
            const std::string name = config.substr(dollar + 2, end - dollar - 2);
            if (isValidName(name)) {
                parts.push_back(ConfigValuePart{true, name});
            } else {
                appendLiteral(parts, config.substr(dollar, end + 1 - dollar));
            }
            index = end + 1;
        } else if (next != '\0' && isNameStart(next)) {
            std::size_t end = dollar + 1;
            while (end < config.size() && isNameChar(config[end])) {
                ++end;
            }
            parts.push_back(ConfigValuePart{true, config.substr(dollar + 1, end - dollar - 1)});
            index = end;
        } else {
            appendLiteral(parts, "$");
            index = dollar + 1;
        }
    }
    return parts;
}

bool ConfigValueResolver::isCommand(const std::string& config) const {
    return !config.empty() && config[0] == '!';
}

std::optional<std::string> ConfigValueResolver::envValue(const std::string& name,
                                                         const Env& env) const {
    const auto found = env.find(name);
    if (found != env.end() && !found->second.empty()) {
        return found->second;
    }
    const auto value = m_environment.get(name);
    if (value && !value->empty()) {
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> ConfigValueResolver::envVarName(const std::string& config) const {
    if (isCommand(config)) {
        return std::nullopt;
    }
    const auto parts = parseTemplate(config);
    if (parts.size() == 1 && parts[0].isEnv) {
        return parts[0].value;
    }
    return std::nullopt;
}

std::vector<std::string> ConfigValueResolver::envVarNames(const std::string& config) const {
    std::vector<std::string> names;
    if (isCommand(config)) {
        return names;
    }
    for (const auto& part : parseTemplate(config)) {
        if (part.isEnv && std::find(names.begin(), names.end(), part.value) == names.end()) {
            names.push_back(part.value);
        }
    }
    return names;
}

std::vector<std::string> ConfigValueResolver::missingEnvVarNames(const std::string& config,
                                                                 const Env& env) const {
    std::vector<std::string> missing;
    for (const auto& name : envVarNames(config)) {
        if (!envValue(name, env)) {
            missing.push_back(name);
        }
    }
    return missing;
}

bool ConfigValueResolver::isConfigured(const std::string& config, const Env& env) const {
    return missingEnvVarNames(config, env).empty();
}

std::optional<std::string> ConfigValueResolver::resolveParts(
    const std::vector<ConfigValuePart>& parts, const Env& env) const {
    std::string resolved;
    for (const auto& part : parts) {
        if (!part.isEnv) {
            resolved += part.value;
            continue;
        }
        const auto value = envValue(part.value, env);
        if (!value) {
            return std::nullopt;
        }
        resolved += *value;
    }
    return resolved;
}

std::optional<std::string> ConfigValueResolver::runCommand(const std::string& commandConfig) {
    ProcessRequest request;
    request.command = "/bin/sh";
    request.args = {"-c", "(" + commandConfig.substr(1) + ") 2>/dev/null"};
    request.timeout = std::chrono::seconds(10);
    auto result = m_processes.run(request);
    if (!result || result->exitCode != 0 || result->timedOut) {
        return std::nullopt;
    }
    std::string text = result->output;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::optional<std::string> ConfigValueResolver::resolveUncached(const std::string& config,
                                                                const Env& env) {
    if (isCommand(config)) {
        return runCommand(config);
    }
    return resolveParts(parseTemplate(config), env);
}

std::optional<std::string> ConfigValueResolver::resolve(const std::string& config, const Env& env) {
    if (!isCommand(config)) {
        return resolveParts(parseTemplate(config), env);
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto cached = m_cache.find(config);
        if (cached != m_cache.end()) {
            return cached->second;
        }
    }
    auto value = runCommand(config);
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_cache[config] = value;
    return value;
}

Result<std::string> ConfigValueResolver::resolveOrError(const std::string& config,
                                                        const std::string& description,
                                                        const Env& env) {
    if (auto value = resolveUncached(config, env)) {
        return *value;
    }
    if (isCommand(config)) {
        return std::unexpected(Error{"config_value", "Failed to resolve " + description +
                                                          " from shell command: " + config.substr(1)});
    }
    const auto missing = missingEnvVarNames(config, env);
    if (missing.size() == 1) {
        return std::unexpected(Error{"config_value", "Failed to resolve " + description +
                                                          " from environment variable: " + missing[0]});
    }
    if (missing.size() > 1) {
        std::string joined;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            joined += (i > 0 ? ", " : "") + missing[i];
        }
        return std::unexpected(Error{"config_value", "Failed to resolve " + description +
                                                          " from environment variables: " + joined});
    }
    return std::unexpected(Error{"config_value", "Failed to resolve " + description});
}

ConfigValueResolver::Headers ConfigValueResolver::resolveHeaders(const Headers& headers,
                                                                 const Env& env) {
    Headers resolved;
    for (const auto& [name, value] : headers) {
        const auto result = resolve(value, env);
        if (result && !result->empty()) {
            resolved[name] = *result;
        }
    }
    return resolved;
}

Result<ConfigValueResolver::Headers> ConfigValueResolver::resolveHeadersOrError(
    const Headers& headers, const std::string& description, const Env& env) {
    Headers resolved;
    for (const auto& [name, value] : headers) {
        auto result = resolveOrError(value, description + " header \"" + name + "\"", env);
        if (!result) {
            return std::unexpected(result.error());
        }
        resolved[name] = *result;
    }
    return resolved;
}

void ConfigValueResolver::clearCache() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
}
