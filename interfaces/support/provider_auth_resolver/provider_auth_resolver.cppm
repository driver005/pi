module;

#include <cstdint>

export module pi.support.provider_auth_resolver;

import std;
export import pi.platform.i_clock;
export import pi.provider.i_credential_store;
export import pi.provider.i_oauth_flow;
export import pi.support.config_value_resolver;
export import pi.support.env_key_table;
export import pi.support.header_merger;
export import pi.types.auth_result;
export import pi.types.json;
export import pi.types.provider_definition;

/**
 * Finds the credentials for a provider at request time. A stored credential owns the provider:
 * ambient sources (environment variables, AWS, ADC) are only consulted when nothing is stored.
 * models.json provider sections add an apiKey reference, headers and authHeader on top.
 * Port of resolveProviderAuth (ai/auth/resolve.ts) and the composed auth in core/provider-composer.ts.
 */
export class ProviderAuthResolver {
public:
    using Env = std::map<std::string, std::string>;

    ProviderAuthResolver(ICredentialStore& credentials, const EnvKeyTable& envKeys, ConfigValueResolver& config, const IClock& clock, std::map<std::string, IOauthFlow*> oauthFlows)
        : m_credentials(credentials),
          m_envKeys(envKeys),
          m_config(config),
          m_clock(clock),
          m_flows(std::move(oauthFlows)) {}

    /**
     * nullopt: the provider has no usable credentials. Error: credentials exist but cannot be
     * used (failed refresh, command failure, missing environment variable).
     */
    Result<std::optional<AuthResult>> resolve(const ProviderDefinition& provider, const Json& providerConfig, const std::optional<std::string>& apiKeyOverride, const Env& envOverride) {
        const bool apiKeyAuth = hasApiKeyAuth(provider, providerConfig);
        const auto flow = m_flows.find(provider.id);
        IOauthFlow* oauth = flow == m_flows.end() ? nullptr : flow->second;

        if (apiKeyOverride && apiKeyAuth) {
            Credential synthetic;
            synthetic.key = *apiKeyOverride;
            if (!envOverride.empty()) {
                synthetic.env = envOverride;
            }
            return resolveApiKey(provider, providerConfig, synthetic, envOverride);
        }
        auto stored = m_credentials.read(provider.id);
        if (!stored) {
            return std::unexpected(Error{"auth", "Credential store read failed for " + provider.id + ": " +
                                                     stored.error().message});
        }
        if (stored->has_value()) {
            Credential credential = **stored;
            if (credential.type == CredentialType::OAuth && oauth != nullptr) {
                return resolveOAuth(provider, providerConfig, credential, *oauth);
            }
            if (credential.type == CredentialType::ApiKey && apiKeyAuth) {
                if (!envOverride.empty()) {
                    Env merged = credential.env.value_or(Env{});
                    for (const auto& [name, value] : envOverride) {
                        merged[name] = value;
                    }
                    credential.env = std::move(merged);
                }
                return resolveApiKey(provider, providerConfig, credential, envOverride);
            }
            return std::optional<AuthResult>();
        }
        if (!apiKeyAuth) {
            return std::optional<AuthResult>();
        }
        return resolveApiKey(provider, providerConfig, std::nullopt, envOverride);
    }

private:
    Result<std::optional<AuthResult>> resolveApiKey(const ProviderDefinition& provider, const Json& config, const std::optional<Credential>& credential, const Env& env) {
        const std::string rawKey = stringField(config, "apiKey");
        const bool inherited = provider.builtin && provider.supportsApiKey;
        Result<std::optional<AuthResult>> result = std::optional<AuthResult>();
        if (credential) {
            if (inherited) {
                result = inheritedApiKey(provider, credential, env);
            } else if (credential->key && !credential->key->empty()) {
                AuthResult stored;
                stored.auth.apiKey = *credential->key;
                stored.env = credential->env.value_or(Env{});
                stored.source = "stored credential";
                result = std::optional<AuthResult>(std::move(stored));
            }
        } else if (!rawKey.empty()) {
            auto key = m_config.resolveOrError(rawKey, "API key for provider \"" + provider.id + "\"",
                                               contextEnv({rawKey}, env));
            if (!key) {
                return std::unexpected(Error{"auth", key.error().message});
            }
            if (inherited) {
                Credential synthetic;
                synthetic.key = *key;
                result = inheritedApiKey(provider, synthetic, env);
            } else {
                AuthResult configured;
                configured.auth.apiKey = *key;
                configured.source = "configured API key";
                result = std::optional<AuthResult>(std::move(configured));
            }
        } else if (inherited) {
            result = inheritedApiKey(provider, std::nullopt, env);
        }
        if (!result) {
            return std::unexpected(result.error());
        }
        if (!result->has_value()) {
            return std::optional<AuthResult>();
        }
        AuthResult out = **result;
        Env headerEnv = credential ? credential->env.value_or(Env{}) : Env{};
        for (const auto& [name, value] : out.env) {
            headerEnv[name] = value;
        }
        auto configured = withConfiguredAuth(out.auth, config, provider.id, headerEnv);
        if (!configured) {
            return std::unexpected(configured.error());
        }
        out.auth = std::move(*configured);
        return std::optional<AuthResult>(std::move(out));
    }

    Result<std::optional<AuthResult>> inheritedApiKey(const ProviderDefinition& provider, const std::optional<Credential>& credential, const Env& env) {
        if (credential && credential->key && !credential->key->empty()) {
            AuthResult result;
            result.auth.apiKey = *credential->key;
            result.env = credential->env.value_or(Env{});
            result.source = "stored credential";
            return std::optional<AuthResult>(std::move(result));
        }
        if (provider.id == "anthropic") {
            if (const auto token = m_envKeys.value("ANTHROPIC_AUTH_TOKEN", env)) {
                AuthResult result;
                result.auth.headers.emplace_back("Authorization", "Bearer " + *token);
                result.source = "ANTHROPIC_AUTH_TOKEN";
                return std::optional<AuthResult>(std::move(result));
            }
        }
        for (const auto& name : provider.envVars) {
            if (provider.id == "anthropic" && name == "ANTHROPIC_AUTH_TOKEN") {
                continue;
            }
            if (const auto value = m_envKeys.value(name, env)) {
                AuthResult result;
                result.auth.apiKey = *value;
                result.source = name;
                return std::optional<AuthResult>(std::move(result));
            }
        }
        if (const auto ambient = m_envKeys.apiKey(provider.id, env); ambient && *ambient == "<authenticated>") {
            AuthResult result;
            result.source = "ambient credentials";
            return std::optional<AuthResult>(std::move(result));
        }
        return std::optional<AuthResult>();
    }

    Result<std::optional<AuthResult>> resolveOAuth(const ProviderDefinition& provider, const Json& config, const Credential& stored, IOauthFlow& flow) {
        constexpr std::int64_t minimumValidityMs = 5 * 60 * 1000;
        Credential credential = stored;
        if (expiresSoon(credential, minimumValidityMs)) {
            auto signal = std::make_shared<AbortSignal>();
            auto post = m_credentials.modify(
                provider.id, [&](const std::optional<Credential>& current) -> Result<std::optional<Credential>> {
                    if (!current || current->type != CredentialType::OAuth) {
                        return std::optional<Credential>();
                    }
                    if (!expiresSoon(*current, minimumValidityMs)) {
                        return std::optional<Credential>();
                    }
                    auto refreshed = flow.refresh(*current, signal);
                    if (!refreshed) {
                        return std::unexpected(Error{"oauth", "OAuth refresh failed for " + provider.id + ": " +
                                                                  refreshed.error().message});
                    }
                    refreshed->type = CredentialType::OAuth;
                    return std::optional<Credential>(*refreshed);
                });
            if (!post) {
                return std::unexpected(post.error());
            }
            if (!post->has_value() || (*post)->type != CredentialType::OAuth) {
                return std::optional<AuthResult>();
            }
            credential = **post;
        }
        AuthResult result;
        result.auth = flow.toAuth(credential);
        result.source = "OAuth";
        Env headerEnv;
        if (credential.extra.is_object() && credential.extra.contains("env") && credential.extra["env"].is_object()) {
            for (const auto& entry : credential.extra["env"].items()) {
                const std::string& name = entry.key();
                const Json& value = entry.value();
                if (value.is_string()) {
                    headerEnv[name] = value.get<std::string>();
                }
            }
        }
        auto configured = withConfiguredAuth(std::move(result.auth), config, provider.id, headerEnv);
        if (!configured) {
            return std::unexpected(configured.error());
        }
        result.auth = std::move(*configured);
        return std::optional<AuthResult>(std::move(result));
    }

    Result<ModelAuth> withConfiguredAuth(ModelAuth auth, const Json& config, const std::string& providerId, const Env& headerEnv) {
        ConfigValueResolver::Headers raw;
        if (config.is_object() && config.contains("headers") && config["headers"].is_object()) {
            for (const auto& entry : config["headers"].items()) {
                const std::string& name = entry.key();
                const Json& value = entry.value();
                if (value.is_string()) {
                    raw[name] = value.get<std::string>();
                }
            }
        }
        std::vector<std::string> values;
        for (const auto& [name, value] : raw) {
            values.push_back(value);
        }
        auto resolved = m_config.resolveHeadersOrError(raw, "provider \"" + providerId + "\"",
                                                       contextEnv(values, headerEnv));
        if (!resolved) {
            return std::unexpected(Error{"auth", resolved.error().message});
        }
        for (const auto& [name, value] : *resolved) {
            m_headers.set(auth.headers, name, value);
        }
        const bool authHeader = config.is_object() && config.contains("authHeader") &&
                                config["authHeader"].is_boolean() && config["authHeader"].get<bool>();
        if (authHeader) {
            if (!auth.apiKey) {
                return std::unexpected(Error{"auth", "authHeader requires a resolved API key"});
            }
            m_headers.set(auth.headers, "Authorization", "Bearer " + *auth.apiKey);
        }
        return auth;
    }

    Env contextEnv(const std::vector<std::string>& values, const Env& explicitEnv) const {
        Env env = explicitEnv;
        for (const auto& value : values) {
            for (const auto& name : m_config.envVarNames(value)) {
                if (env.contains(name)) {
                    continue;
                }
                if (const auto found = m_envKeys.value(name, {})) {
                    env[name] = *found;
                }
            }
        }
        return env;
    }

    bool expiresSoon(const Credential& credential, std::int64_t minimumValidityMs) const {
        return static_cast<double>(m_clock.nowMs() + minimumValidityMs) >= credential.expires;
    }

    std::string stringField(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return "";
    }

    bool hasApiKeyAuth(const ProviderDefinition& provider, const Json& config) const {
        if (!provider.builtin || provider.supportsApiKey) {
            return true;
        }
        // OAuth-only providers get no fabricated API-key method, unless models.json configures a key.
        return !stringField(config, "apiKey").empty();
    }

    ICredentialStore& m_credentials;
    const EnvKeyTable& m_envKeys;
    ConfigValueResolver& m_config;
    const IClock& m_clock;
    std::map<std::string, IOauthFlow*> m_flows;
    HeaderMerger m_headers;
};
