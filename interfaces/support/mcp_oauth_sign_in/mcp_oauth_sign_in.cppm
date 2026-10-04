export module pi.support.mcp_oauth_sign_in;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_callback_server;
export import pi.platform.i_clock;
export import pi.platform.i_crypto;
export import pi.platform.i_http_client;
export import pi.support.mcp_oauth_refresher;
export import pi.support.mcp_oauth_store;
export import pi.types.callback_address;

/**
 * Signs in to a remote MCP server with the OAuth authorization code flow (PKCE, RFC 7636) and stores the credentials in
 * `mcp-auth.json` for McpOauthTokenProvider: refreshes the stored grant when it can, otherwise discovers the authorization
 * server, uses the configured or registers a client (dynamic client registration, RFC 7591), sends the user to the
 * authorization URL (shown through the prompt callback; the browser comes back to a loopback callback server), checks the
 * issuer of the response (RFC 9207) and exchanges the code for tokens. A server asking for more scope (`insufficient_scope`)
 * skips the refresh and requests the granted and the challenged scopes together. Client ID Metadata Documents and pasting a
 * redirect URL are not ported. Port of signInMcpServer in extensions/mcp/oauth.ts and the authorization half of
 * packages/mcp/src/oauth/flow.ts.
 *
 * Errors: "oauth:<code>" for errors the authorization server reported, "oauth_registration" when the server cannot register
 * a client, "oauth_issuer" for an issuer mismatch, plus the discovery, HTTP and callback server codes.
 */
export class McpOauthSignIn {
public:
    using CallbackFactory = std::function<std::unique_ptr<ICallbackServer>()>;
    using ShowUrl = std::function<void(const std::string& authorizationUrl)>;

    static constexpr std::string_view kCallbackPath = "/callback";
    static constexpr std::chrono::milliseconds kDefaultTimeout{5 * 60 * 1000};

    McpOauthSignIn(McpOauthStore& store, McpOauthRefresher& refresher, IHttpClient& http, const ICrypto& crypto, const IBase64Codec& base64, CallbackFactory callbacks)
        : m_store(store),
          m_refresher(refresher),
          m_http(http),
          m_crypto(crypto),
          m_base64(base64),
          m_callbacks(std::move(callbacks)) {}

    /**
     * Signs in to `name` at `serverUrl`. `settings` is the server's `oauth` configuration (`clientId`, resolved
     * `clientSecret`, `scope`, `clientName`, `callbackPort`, `callbackUrl`, `authServerMetadataUrl`, `clientRegistration`);
     * `challenge` the last `WWW-Authenticate` challenge (`resourceMetadataUrl`, `scope`, `error`) or null.
     */
    Result<void> signIn(const std::string& name, const std::string& serverUrl, const Json& settings, const Json& challenge, const ShowUrl& showUrl, std::chrono::milliseconds timeout, const std::shared_ptr<AbortSignal>& signal) {
        if (settings.value("clientRegistration", std::string()) == "cimd") {
            return std::unexpected(Error{"oauth_registration", "oauth.clientRegistration \"cimd\" is not supported here; use dynamic registration or oauth.clientId"});
        }
        auto loaded = m_store.load(name, serverUrl);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        auto normalized = m_urls.normalize(serverUrl);
        if (!normalized) {
            return std::unexpected(normalized.error());
        }
        Json state = loaded->value_or(Json{{"serverUrl", *normalized}});
        const bool stepUp = challenge.is_object() && challenge.value("error", std::string()) == "insufficient_scope";
        const std::optional<std::string> metadataUrl = challenge.is_object() && challenge.contains("resourceMetadataUrl") ? std::optional<std::string>(challenge["resourceMetadataUrl"].get<std::string>()) : std::nullopt;

        const std::unique_ptr<ICallbackServer> callback = m_callbacks();
        auto where = callbackSettings(settings);
        if (!where) {
            return std::unexpected(where.error());
        }
        const int preferred = where->port > 0 ? where->port : registeredPort(state);
        auto port = callback->listen(where->host, preferred, where->port > 0);
        if (!port) {
            return std::unexpected(port.error());
        }
        const std::string redirect = where->fixedRedirect.empty() ? "http://" + where->redirectHost + ":" + std::to_string(*port) + where->path : where->fixedRedirect;
        auto done = authorize(name, serverUrl, settings, metadataUrl, stepUp, challenge, redirect, where->path, *callback, state, showUrl, timeout, signal);
        callback->close();
        return done;
    }

private:
    Result<void> authorize(const std::string& name, const std::string& serverUrl, const Json& settings, const std::optional<std::string>& metadataUrl, bool stepUp, const Json& challenge, const std::string& redirect, const std::string& callbackPath, ICallbackServer& callback, Json state, const ShowUrl& showUrl, std::chrono::milliseconds timeout, const std::shared_ptr<AbortSignal>& signal) {
        // Every sign-in gets a fresh state; a registered client cannot use another redirect URI and its tokens belong to it.
        state.erase("oauthState");
        const bool keepClient = !settings.value("clientId", std::string()).empty() || registeredRedirects(state).contains(redirect);
        if (!keepClient) {
            state.erase("clientInformation");
            state.erase("tokens");
            state.erase("tokensExpireAt");
        }
        if (auto saved = m_store.save(name, serverUrl, state); !saved) {
            return saved;
        }
        // A refresh keeps the granted scope: more scope needs the browser.
        if (!stepUp && state.contains("tokens")) {
            auto refreshed = m_refresher.refresh(serverUrl, state, settings, metadataUrl, signal);
            if (refreshed) {
                return m_store.save(name, serverUrl, *refreshed);
            }
            if (refreshed.error().code == "oauth_insecure" || (refreshed.error().code.starts_with("oauth:") && refreshed.error().code != "oauth:server_error")) {
                return std::unexpected(refreshed.error());
            }
        }
        auto discovered = m_refresher.discoverServer(serverUrl, state, settings, metadataUrl, signal);
        if (!discovered) {
            return std::unexpected(discovered.error());
        }
        auto resource = m_refresher.resourceFor(serverUrl, *discovered);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        const Json metadata = discovered->value("authorizationServerMetadata", Json());
        const std::string scope = requestedScope(settings, stepUp, challenge, state, *discovered);
        auto client = clientFor(settings, state, *discovered, metadata, redirect, scope, signal);
        if (!client) {
            return std::unexpected(client.error());
        }
        state["clientInformation"] = *client;

        const std::string verifier = m_base64.encodeUrl(m_crypto.randomBytes(32));
        const std::string challengeCode = m_base64.encodeUrl(m_crypto.sha256(verifier));
        std::string authState;
        for (const char byte : m_crypto.randomBytes(32)) {
            authState += std::format("{:02x}", static_cast<unsigned char>(byte));
        }
        auto url = authorizationUrl(*discovered, metadata, *client, redirect, scope, authState, challengeCode, *resource);
        if (!url) {
            return std::unexpected(url.error());
        }
        state["codeVerifier"] = verifier;
        state["oauthState"] = authState;
        if (auto saved = m_store.save(name, serverUrl, state); !saved) {
            return saved;
        }
        showUrl(*url);
        auto response = callback.waitForCallback({callbackPath}, authState, timeout, signal);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->contains("error")) {
            const std::string code = (*response)["error"].get<std::string>();
            return std::unexpected(Error{"oauth:" + code, response->value("error_description", code)});
        }
        if (auto issuer = checkIssuer(metadata, *response); !issuer) {
            return issuer;
        }
        auto tokens = m_refresher.requestTokens(*discovered, *client, *resource,
                                                {{"grant_type", "authorization_code"}, {"code", (*response)["code"].get<std::string>()}, {"code_verifier", verifier}, {"redirect_uri", redirect}}, signal);
        if (!tokens) {
            return std::unexpected(tokens.error());
        }
        state.erase("codeVerifier");
        state.erase("oauthState");
        // A response without `scope` grants the requested scope (RFC 6749, 5.1).
        return m_store.save(name, serverUrl, m_refresher.withTokens(std::move(state), *tokens, scope.empty() ? std::nullopt : std::optional<std::string>(scope)));
    }

    /** The scopes to request: the configured and challenged ones (plus the granted ones on a step-up), else what the server lists. */
    std::string requestedScope(const Json& settings, bool stepUp, const Json& challenge, const Json& state, const Json& discovered) const {
        const std::string challenged = challenge.is_object() ? challenge.value("scope", std::string()) : std::string();
        const std::string granted = state.contains("tokens") && state["tokens"].is_object() ? state["tokens"].value("scope", std::string()) : std::string();
        std::string merged = mergeScopes({settings.value("scope", std::string()), stepUp && !challenged.empty() ? granted : std::string(), challenged});
        if (!merged.empty()) {
            return merged;
        }
        const Json resource = discovered.value("resourceMetadata", Json());
        if (resource.is_object() && resource.contains("scopes_supported") && resource["scopes_supported"].is_array()) {
            std::vector<std::string> supported;
            for (const Json& entry : resource["scopes_supported"]) {
                if (entry.is_string()) {
                    supported.push_back(entry.get<std::string>());
                }
            }
            return join(supported);
        }
        return "";
    }

    std::string mergeScopes(const std::vector<std::string>& lists) const {
        std::vector<std::string> merged;
        for (const std::string& list : lists) {
            std::istringstream words(list);
            std::string word;
            while (words >> word) {
                if (std::ranges::find(merged, word) == merged.end()) {
                    merged.push_back(word);
                }
            }
        }
        return join(merged);
    }

    std::string join(const std::vector<std::string>& words) const {
        std::string out;
        for (const std::string& word : words) {
            out += (out.empty() ? "" : " ") + word;
        }
        return out;
    }

    /** The configured client, the registered one, or a newly registered one. */
    Result<Json> clientFor(const Json& settings, const Json& state, const Json& discovered, const Json& metadata, const std::string& redirect, const std::string& scope, const std::shared_ptr<AbortSignal>& signal) {
        const Json existing = m_refresher.clientFor(state, settings);
        if (!existing.is_null()) {
            return existing;
        }
        std::string endpoint;
        if (metadata.is_object()) {
            if (!metadata.contains("registration_endpoint") || !metadata["registration_endpoint"].is_string()) {
                return std::unexpected(Error{"oauth_registration", "Authorization server does not support dynamic client registration; set oauth.clientId"});
            }
            endpoint = metadata["registration_endpoint"].get<std::string>();
        } else {
            auto server = m_urls.parse(discovered["authorizationServerUrl"].get<std::string>());
            if (!server) {
                return std::unexpected(Error{"oauth_discovery", server.error().message});
            }
            endpoint = m_urls.origin(*server) + "/register";
        }
        const bool secret = !settings.value("clientSecret", std::string()).empty();
        Json body{{"client_name", settings.value("clientName", std::string("pi"))}, {"redirect_uris", Json::array({redirect})}};
        body["grant_types"] = Json::array({"authorization_code", "refresh_token"});
        body["response_types"] = Json::array({"code"});
        body["token_endpoint_auth_method"] = secret ? "client_secret_post" : "none";
        if (!scope.empty()) {
            body["scope"] = scope;
        }
        HttpRequest request;
        request.method = "POST";
        request.url = endpoint;
        request.headers = {{"Accept", "application/json"}, {"content-type", "application/json"}};
        request.body = body.dump();
        request.timeout = std::chrono::milliseconds(15000);
        request.signal = signal;
        const auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"oauth_registration", "OAuth dynamic client registration failed with status " + std::to_string(response->status) + ": " + response->body});
        }
        Json registered = Json::parse(response->body, nullptr, false);
        if (!registered.is_object() || !registered.contains("client_id") || !registered["client_id"].is_string() || registered["client_id"].get<std::string>().empty()) {
            return std::unexpected(Error{"oauth_registration", "Invalid OAuth client registration response"});
        }
        if (!registered.contains("redirect_uris")) {
            registered["redirect_uris"] = Json::array({redirect});
        }
        return registered;
    }

    Result<std::string> authorizationUrl(const Json& discovered, const Json& metadata, const Json& client, const std::string& redirect, const std::string& scope, const std::string& state, const std::string& challenge, const std::optional<std::string>& resource) const {
        std::string base;
        if (metadata.is_object()) {
            if (!supports(metadata, "response_types_supported", "code")) {
                return std::unexpected(Error{"oauth_discovery", "Authorization server does not support authorization codes"});
            }
            if (metadata.contains("code_challenge_methods_supported") && !supports(metadata, "code_challenge_methods_supported", "S256")) {
                return std::unexpected(Error{"oauth_discovery", "Authorization server does not support PKCE S256"});
            }
            base = metadata["authorization_endpoint"].get<std::string>();
        } else {
            auto server = m_urls.parse(discovered["authorizationServerUrl"].get<std::string>());
            if (!server) {
                return std::unexpected(Error{"oauth_discovery", server.error().message});
            }
            base = m_urls.origin(*server) + "/authorize";
        }
        std::vector<std::pair<std::string, std::string>> params{{"response_type", "code"},
                                                                {"client_id", client["client_id"].get<std::string>()},
                                                                {"code_challenge", challenge},
                                                                {"code_challenge_method", "S256"},
                                                                {"redirect_uri", redirect},
                                                                {"state", state}};
        if (!scope.empty()) {
            params.emplace_back("scope", scope);
            std::istringstream words(scope);
            std::string word;
            while (words >> word) {
                if (word == "offline_access") {
                    params.emplace_back("prompt", "consent");
                    break;
                }
            }
        }
        if (resource) {
            params.emplace_back("resource", *resource);
        }
        std::string url = base + (base.find('?') == std::string::npos ? "?" : "&");
        for (std::size_t i = 0; i < params.size(); ++i) {
            url += (i == 0 ? "" : "&") + m_urls.encode(params[i].first) + "=" + m_urls.encode(params[i].second);
        }
        return url;
    }

    bool supports(const Json& metadata, const char* key, const std::string& value) const {
        if (!metadata.contains(key) || !metadata[key].is_array()) {
            return false;
        }
        return std::ranges::any_of(metadata[key], [&](const Json& entry) { return entry.is_string() && entry.get<std::string>() == value; });
    }

    /** RFC 9207: a code from another authorization server must never be sent to this one. */
    Result<void> checkIssuer(const Json& metadata, const Json& response) const {
        if (!metadata.is_object()) {
            return {};
        }
        const bool given = response.contains("iss");
        if (given || metadata.value("authorization_response_iss_parameter_supported", false)) {
            const std::string received = given ? response["iss"].get<std::string>() : std::string();
            if (!given || received != metadata["issuer"].get<std::string>()) {
                return std::unexpected(Error{"oauth_issuer", "OAuth issuer mismatch: expected \"" + metadata["issuer"].get<std::string>() + "\", received " + (given ? "\"" + received + "\"" : std::string("none"))});
            }
        }
        return {};
    }

    /** Where the callback server listens, from `callbackUrl` and `callbackPort` (default `http://127.0.0.1/callback`). */
    Result<CallbackAddress> callbackSettings(const Json& settings) const {
        const std::string configured = settings.value("callbackUrl", std::string());
        auto url = m_urls.parse(configured.empty() ? "http://127.0.0.1/callback" : configured);
        if (!url) {
            return std::unexpected(Error{"oauth_settings", "oauth.callbackUrl: " + url.error().message});
        }
        CallbackAddress address;
        const std::string bare = url->host.starts_with("[") ? url->host.substr(1, url->host.size() - 2) : url->host;
        address.host = bare == "localhost" ? "127.0.0.1" : bare;
        address.redirectHost = bare.find(':') == std::string::npos ? bare : "[" + bare + "]";
        address.path = url->path;
        address.port = url->port ? *url->port : settings.value("callbackPort", 0);
        if (url->port) {
            // A configured URI with a port is sent as written, since servers compare it as a string.
            address.fixedRedirect = configured;
        } else if (address.port > 0) {
            ParsedUrl fixed = *url;
            fixed.port = address.port;
            fixed.query.clear();
            fixed.fragment.clear();
            address.fixedRedirect = m_urls.print(fixed);
        }
        return address;
    }

    std::set<std::string> registeredRedirects(const Json& state) const {
        std::set<std::string> out;
        const Json client = state.value("clientInformation", Json());
        if (client.is_object() && client.contains("redirect_uris") && client["redirect_uris"].is_array()) {
            for (const Json& uri : client["redirect_uris"]) {
                if (uri.is_string()) {
                    out.insert(uri.get<std::string>());
                }
            }
        }
        return out;
    }

    int registeredPort(const Json& state) const {
        const Json client = state.value("clientInformation", Json());
        if (client.is_object() && client.contains("redirect_uris") && client["redirect_uris"].is_array() && !client["redirect_uris"].empty() && client["redirect_uris"][0].is_string()) {
            if (auto url = m_urls.parse(client["redirect_uris"][0].get<std::string>()); url && url->port) {
                return *url->port;
            }
        }
        return 0;
    }

    McpOauthStore& m_store;
    McpOauthRefresher& m_refresher;
    IHttpClient& m_http;
    const ICrypto& m_crypto;
    const IBase64Codec& m_base64;
    CallbackFactory m_callbacks;
    UrlParser m_urls;
};
