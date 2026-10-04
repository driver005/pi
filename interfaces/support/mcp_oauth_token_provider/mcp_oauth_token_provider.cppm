export module pi.support.mcp_oauth_token_provider;

import std;
export import pi.platform.i_clock;
export import pi.support.mcp_oauth_refresher;
export import pi.support.mcp_oauth_store;

/**
 * The bearer token of one remote MCP server that authenticates with OAuth: the access token stored in `mcp-auth.json`
 * (by a sign-in done with the TypeScript CLI), refreshed before it is sent when it is about to expire and again after a
 * 401. A connection never starts a browser flow: when only a new sign-in helps (no refresh token, a rejected grant, or a
 * server asking for more scope with `insufficient_scope`), onUnauthorized() answers false and the caller reports that the
 * server needs sign-in.
 *
 * Refresh tokens are often single-use, so two refreshes with the same token lose the grant. Calls in this process share one
 * refresh, other processes are kept out by the store's refresh lock (held from reading the tokens to saving new ones), and
 * tokens that changed meanwhile are used without refreshing. Port of createMcpAuthProvider in extensions/mcp/oauth.ts.
 */
export class McpOauthTokenProvider {
public:
    /** The server's `oauth` settings (`clientId`, resolved `clientSecret`, `authServerMetadataUrl`); read when a refresh needs them. */
    using Settings = std::function<Result<Json>()>;

    /** Access tokens this close to expiry are refreshed before they are sent. */
    static constexpr std::int64_t kRefreshSkewMs = 30000;

    McpOauthTokenProvider(std::string name, std::string serverUrl, McpOauthStore& store, McpOauthRefresher& refresher, const IClock& clock, Settings settings)
        : m_name(std::move(name)),
          m_serverUrl(std::move(serverUrl)),
          m_store(store),
          m_refresher(refresher),
          m_clock(clock),
          m_settings(std::move(settings)) {}

    /** The access token to send, empty when there is none. */
    std::string token() {
        const std::lock_guard<std::mutex> serial(m_mutex);
        const std::optional<Json> state = load();
        const std::string current = accessToken(state);
        if (!state || !expired(*state) || refreshToken(*state).empty()) {
            return current;
        }
        // Failures fall through: the request goes out with the old token and a 401 decides what happens.
        (void)refreshLocked(current, std::nullopt);
        return accessToken(load());
    }

    /**
     * Handles a 401 whose `WWW-Authenticate` header is `wwwAuthenticate` for a request that carried `staleToken`: refreshes the
     * token. True when the request should be retried with the stored token; false when the user has to sign in.
     */
    bool onUnauthorized(const std::string& wwwAuthenticate, const std::string& staleToken) {
        const Json parsed = parseChallenge(wwwAuthenticate);
        const std::lock_guard<std::mutex> serial(m_mutex);
        m_challenge = parsed;
        // A refresh keeps the granted scope, so more scope needs a new sign-in.
        if (parsed.value("error", std::string()) == "insufficient_scope") {
            return false;
        }
        std::optional<std::string> metadataUrl;
        if (parsed.contains("resourceMetadataUrl")) {
            metadataUrl = parsed["resourceMetadataUrl"].get<std::string>();
        }
        return refreshLocked(staleToken, metadataUrl).has_value();
    }

    /** The last `WWW-Authenticate` challenge: `{resourceMetadataUrl?, scope?, error?, errorDescription?}`; null before any. */
    Json challenge() const {
        const std::lock_guard<std::mutex> serial(m_mutex);
        return m_challenge;
    }

    /** Parses a `WWW-Authenticate` header; `{}` unless it is a Bearer or DPoP challenge. */
    Json parseChallenge(const std::string& header) const {
        Json out = Json::object();
        std::size_t start = header.find_first_not_of(" \t");
        if (start == std::string::npos) {
            return out;
        }
        std::string scheme = header.substr(start, header.find_first_of(" \t", start) - start);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (scheme != "bearer" && scheme != "dpop") {
            return out;
        }
        const std::pair<const char*, const char*> fields[] = {{"resource_metadata", "resourceMetadataUrl"}, {"scope", "scope"}, {"error", "error"}, {"error_description", "errorDescription"}};
        for (const auto& [name, key] : fields) {
            const std::regex pattern(std::string("(?:^|[,\\s])") + name + "=(?:\"([^\"]*)\"|([^\\s,]+))", std::regex::icase);
            std::smatch match;
            if (!std::regex_search(header, match, pattern)) {
                continue;
            }
            const std::string value = match[1].matched && match[1].length() > 0 ? match[1].str() : match[2].matched ? match[2].str() : std::string();
            if (!value.empty()) {
                out[key] = value;
            }
        }
        return out;
    }

private:
    /** Replaces `staleToken`, the access token that expired or was rejected. The caller holds the in-process lock. */
    Result<void> refreshLocked(const std::string& staleToken, const std::optional<std::string>& resourceMetadataUrl) {
        return m_store.withRefreshLock(m_name, m_serverUrl, [&]() -> Result<void> {
            auto loaded = m_store.load(m_name, m_serverUrl);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            if (!loaded->has_value() || accessToken(*loaded) != staleToken) {
                // Another process refreshed or the user signed in meanwhile.
                return loaded->has_value() ? Result<void>() : std::unexpected(signIn());
            }
            if (refreshToken(**loaded).empty()) {
                return std::unexpected(signIn());
            }
            auto settings = m_settings();
            if (!settings) {
                return std::unexpected(settings.error());
            }
            auto refreshed = m_refresher.refresh(m_serverUrl, **loaded, *settings, resourceMetadataUrl, nullptr);
            if (!refreshed) {
                return rejected(**loaded, refreshed.error());
            }
            return m_store.save(m_name, m_serverUrl, *refreshed);
        });
    }

    /** A rejected grant leaves no usable tokens behind, so the stored ones are dropped. */
    Result<void> rejected(Json state, const Error& error) {
        if (error.code == "auth_required" && state.contains("tokens")) {
            state.erase("tokens");
            state.erase("tokensExpireAt");
            (void)m_store.save(m_name, m_serverUrl, state);
        }
        return std::unexpected(error);
    }

    std::optional<Json> load() {
        auto loaded = m_store.load(m_name, m_serverUrl);
        return loaded ? *loaded : std::nullopt;
    }

    bool expired(const Json& state) const {
        return state.contains("tokensExpireAt") && state["tokensExpireAt"].is_number() &&
               state["tokensExpireAt"].get<std::int64_t>() - kRefreshSkewMs <= m_clock.nowMs();
    }

    std::string accessToken(const std::optional<Json>& state) const {
        if (!state || !state->contains("tokens") || !(*state)["tokens"].is_object()) {
            return "";
        }
        const Json& tokens = (*state)["tokens"];
        return tokens.contains("access_token") && tokens["access_token"].is_string() ? tokens["access_token"].get<std::string>() : std::string();
    }

    std::string refreshToken(const Json& state) const {
        if (!state.contains("tokens") || !state["tokens"].is_object()) {
            return "";
        }
        const Json& tokens = state["tokens"];
        return tokens.contains("refresh_token") && tokens["refresh_token"].is_string() ? tokens["refresh_token"].get<std::string>() : std::string();
    }

    Error signIn() const {
        return Error{"auth_required", "MCP OAuth authorization requires user interaction"};
    }

    std::string m_name;
    std::string m_serverUrl;
    McpOauthStore& m_store;
    McpOauthRefresher& m_refresher;
    const IClock& m_clock;
    Settings m_settings;
    mutable std::mutex m_mutex;
    Json m_challenge;
};
