export module pi.support.mcp_oauth_providers;

import std;
export import pi.support.config_value_resolver;
export import pi.support.mcp_oauth_token_provider;
export import pi.types.mcp_server_config;

/**
 * The OAuth token providers of the MCP servers of a session, one per server and URL, made on first use and kept so a
 * provider's refresh state survives reconnects. HTTP servers authenticate with OAuth unless the configuration gives an
 * `Authorization` header or an `auth.provider`. The `oauth` settings of an entry (`clientId`, `clientSecret`,
 * `authServerMetadataUrl`) are read when a refresh needs them, so a secret that does not resolve fails the refresh and not the
 * connection. Port of the provider choice in extensions/mcp/runtime.ts.
 */
export class McpOauthProviders {
public:
    McpOauthProviders(McpOauthStore& store, McpOauthRefresher& refresher, const IClock& clock, ConfigValueResolver& resolver)
        : m_store(store),
          m_refresher(refresher),
          m_clock(clock),
          m_resolver(resolver) {}

    /** Whether the server authenticates with OAuth. */
    bool usesOauth(const McpServerConfig& config) const {
        if (!config.http || config.authProvider) {
            return false;
        }
        return std::none_of(config.headers.begin(), config.headers.end(), [](const auto& header) {
            std::string name = header.first;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name == "authorization";
        });
    }

    /** The provider of an OAuth server (see usesOauth). */
    std::shared_ptr<McpOauthTokenProvider> providerFor(const McpServerConfig& config) {
        const std::string key = config.name + '\0' + config.url;
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_providers.find(key);
        if (found != m_providers.end()) {
            return found->second;
        }
        auto provider = std::make_shared<McpOauthTokenProvider>(config.name, config.url, m_store, m_refresher, m_clock, [this, config]() { return settings(config); });
        m_providers.emplace(key, provider);
        return provider;
    }

private:
    Result<Json> settings(const McpServerConfig& config) {
        Json out = Json::object();
        const Json oauth = config.raw.is_object() && config.raw.contains("oauth") ? config.raw["oauth"] : Json();
        if (!oauth.is_object()) {
            return out;
        }
        if (oauth.contains("clientId") && oauth["clientId"].is_string()) {
            out["clientId"] = oauth["clientId"];
        }
        if (oauth.contains("authServerMetadataUrl") && oauth["authServerMetadataUrl"].is_string()) {
            out["authServerMetadataUrl"] = oauth["authServerMetadataUrl"];
        }
        if (oauth.contains("clientSecret") && oauth["clientSecret"].is_string()) {
            auto secret = m_resolver.resolveOrError(oauth["clientSecret"].get<std::string>(), "MCP server \"" + config.name + "\" oauth.clientSecret", {});
            if (!secret) {
                return std::unexpected(secret.error());
            }
            out["clientSecret"] = *secret;
        }
        return out;
    }

    McpOauthStore& m_store;
    McpOauthRefresher& m_refresher;
    const IClock& m_clock;
    ConfigValueResolver& m_resolver;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<McpOauthTokenProvider>> m_providers;
};
