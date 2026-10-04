export module pi.support.mcp_oauth_refresher;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.support.header_merger;
export import pi.support.url_parser;
export import pi.types.json;
export import pi.types.result;

/**
 * Refreshes the access token of a remote MCP server from its stored OAuth state (the `mcp-auth.json` entry): finds the
 * authorization server (the cached discovery, a configured metadata URL, or protected-resource and authorization-server
 * metadata discovery, with issuer validation), then sends a `refresh_token` grant to its token endpoint with the stored (or
 * configured) client, authenticating as the server's metadata allows (RFC 6749, RFC 8707 `resource`, RFC 9728 discovery).
 * The discovery, client selection and token request steps are public because the interactive sign-in
 * (McpOauthSignIn) uses them for its authorization code exchange. Port of the refresh path of runFlow/authorizeMcp and of
 * tokenRequest in packages/mcp/src/oauth/flow.ts.
 *
 * Errors: "auth_required" when only a new sign-in helps (no refresh token or client, `invalid_grant`, `invalid_client`),
 * "oauth:<code>" for other OAuth errors the server reported, "oauth_insecure" for a non-HTTPS endpoint, "oauth_discovery"
 * for metadata problems, and the HTTP client's own codes for transport failures.
 */
export class McpOauthRefresher {
public:
    static constexpr std::string_view kProtocolVersion = "2025-11-25";

    McpOauthRefresher(IHttpClient& http, const IClock& clock, const IBase64Codec& base64)
        : m_http(http),
          m_clock(clock),
          m_base64(base64) {}

    /**
     * The state with new tokens (and the discovery that was used) saved into it. `settings` holds the optional `clientId`,
     * `clientSecret` (already resolved) and `authServerMetadataUrl` of the server's `oauth` configuration.
     */
    Result<Json> refresh(const std::string& serverUrl, Json state, const Json& settings, const std::optional<std::string>& resourceMetadataUrl, const std::shared_ptr<AbortSignal>& signal) {
        const Json existing = state.value("tokens", Json());
        if (!existing.is_object() || !existing.contains("refresh_token") || !existing["refresh_token"].is_string()) {
            return std::unexpected(signIn());
        }
        auto discovered = discoverServer(serverUrl, state, settings, resourceMetadataUrl, signal);
        if (!discovered) {
            return std::unexpected(discovered.error());
        }
        auto resource = resourceFor(serverUrl, *discovered);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        const Json client = clientFor(state, settings);
        if (client.is_null()) {
            return std::unexpected(signIn());
        }
        const std::string refreshToken = existing["refresh_token"].get<std::string>();
        auto tokens = requestTokens(*discovered, client, *resource, {{"grant_type", "refresh_token"}, {"refresh_token", refreshToken}}, signal);
        if (!tokens) {
            return failed(tokens.error());
        }
        // A server that does not rotate the refresh token keeps the old one valid.
        Json merged = Json::object({{"refresh_token", existing["refresh_token"]}});
        for (const auto& entry : tokens->items()) {
            merged[entry.key()] = entry.value();
        }
        return withTokens(std::move(state), merged, existing.contains("scope") ? std::optional<std::string>(existing["scope"].get<std::string>()) : std::nullopt);
    }

    /**
     * `{authorizationServerUrl, authorizationServerMetadata?, resourceMetadata?}` for the server: the cached discovery in
     * `state["discovery"]`, a configured `authServerMetadataUrl`, or metadata discovery (stored into `state` when it ran).
     */
    Result<Json> discoverServer(const std::string& serverUrl, Json& state, const Json& settings, const std::optional<std::string>& resourceMetadataUrl, const std::shared_ptr<AbortSignal>& signal) {
        m_signal = signal;
        return discover(serverUrl, state, settings, resourceMetadataUrl);
    }

    /** The resource indicator for token requests: the protected resource's identifier when it covers the server URL. */
    Result<std::optional<std::string>> resourceFor(const std::string& serverUrl, const Json& discovered) const {
        return selectResource(serverUrl, discovered.value("resourceMetadata", Json()));
    }

    /** The configured client (`clientId`, `clientSecret`), else the one registered in `state`; null when there is none. */
    Json clientFor(const Json& state, const Json& settings) const {
        return clientOf(state, settings);
    }

    /**
     * Sends a token request with `params` (grant type and its arguments) for `client` to the token endpoint of the
     * discovered authorization server, adding the resource indicator and the client's authentication. The parsed token
     * response `{access_token, token_type, expires_in?, scope?, refresh_token?, id_token?}`, or the OAuth error as
     * "oauth:<code>".
     */
    Result<Json> requestTokens(const Json& discovered, const Json& client, const std::optional<std::string>& resource, const std::vector<std::pair<std::string, std::string>>& params, const std::shared_ptr<AbortSignal>& signal) {
        m_signal = signal;
        return tokenRequest(discovered, client, resource, params);
    }

    /** `state` with `tokens` saved: a scope the response lacks defaults to `defaultScope`, and the expiry time follows `expires_in`. */
    Json withTokens(Json state, Json tokens, const std::optional<std::string>& defaultScope) const {
        if (!tokens.contains("scope") && defaultScope && !defaultScope->empty()) {
            tokens["scope"] = *defaultScope;
        }
        state["tokens"] = tokens;
        if (tokens.contains("expires_in") && tokens["expires_in"].is_number()) {
            state["tokensExpireAt"] = m_clock.nowMs() + static_cast<std::int64_t>(tokens["expires_in"].get<double>() * 1000.0);
        } else {
            state.erase("tokensExpireAt");
        }
        return state;
    }

private:
    /** `{authorizationServerUrl, authorizationServerMetadata?, resourceMetadata?}`; stored into `state` when learned. */
    Result<Json> discover(const std::string& serverUrl, Json& state, const Json& settings, const std::optional<std::string>& resourceMetadataUrl) {
        const std::string configured = settings.value("authServerMetadataUrl", std::string());
        if (!configured.empty()) {
            return discoverFromMetadataUrl(serverUrl, configured, resourceMetadataUrl);
        }
        const Json cached = state.value("discovery", Json());
        if (cached.is_object() && cached.contains("authorizationServerUrl") && cached["authorizationServerUrl"].is_string()) {
            Json out = cached;
            if (!out.contains("authorizationServerMetadata")) {
                auto metadata = authorizationServerMetadata(cached["authorizationServerUrl"].get<std::string>());
                if (!metadata) {
                    return std::unexpected(metadata.error());
                }
                if (!metadata->is_null()) {
                    out["authorizationServerMetadata"] = *metadata;
                }
            }
            return out;
        }
        auto info = discoverInfo(serverUrl, resourceMetadataUrl);
        if (!info) {
            return info;
        }
        Json saved = *info;
        if (resourceMetadataUrl) {
            saved["resourceMetadataUrl"] = *resourceMetadataUrl;
        }
        state["discovery"] = saved;
        return info;
    }

    Result<Json> discoverFromMetadataUrl(const std::string& serverUrl, const std::string& metadataUrl, const std::optional<std::string>& resourceMetadataUrl) {
        auto parsed = m_urls.parse(metadataUrl);
        if (!parsed) {
            return std::unexpected(Error{"oauth_discovery", parsed.error().message});
        }
        if (auto secure = secureEndpoint(*parsed); !secure) {
            return std::unexpected(secure.error());
        }
        auto document = fetchJson(metadataUrl);
        if (!document) {
            return std::unexpected(document.error());
        }
        auto metadata = validMetadata(*document);
        if (!metadata) {
            return std::unexpected(metadata.error());
        }
        Json out = Json{{"authorizationServerUrl", (*document)["issuer"]}, {"authorizationServerMetadata", *document}};
        auto resource = protectedResource(serverUrl, resourceMetadataUrl);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        if (!resource->is_null()) {
            out["resourceMetadata"] = *resource;
        }
        return out;
    }

    Result<Json> discoverInfo(const std::string& serverUrl, const std::optional<std::string>& resourceMetadataUrl) {
        auto found = protectedResource(serverUrl, resourceMetadataUrl);
        if (!found) {
            return std::unexpected(found.error());
        }
        const Json resource = *found;
        std::string authorizationServer;
        if (resource.is_object() && resource.contains("authorization_servers") && resource["authorization_servers"].is_array() &&
            !resource["authorization_servers"].empty() && resource["authorization_servers"][0].is_string()) {
            authorizationServer = resource["authorization_servers"][0].get<std::string>();
        } else {
            auto url = m_urls.parse(serverUrl);
            if (!url) {
                return std::unexpected(Error{"oauth_discovery", url.error().message});
            }
            authorizationServer = m_urls.origin(*url) + "/";
        }
        auto metadata = authorizationServerMetadata(authorizationServer);
        if (!metadata) {
            return std::unexpected(metadata.error());
        }
        Json out = Json{{"authorizationServerUrl", authorizationServer}};
        if (!metadata->is_null()) {
            out["authorizationServerMetadata"] = *metadata;
        }
        if (resource.is_object()) {
            out["resourceMetadata"] = resource;
        }
        return out;
    }

    /** The protected resource metadata, null when the server has none (misses are not errors). */
    Result<Json> protectedResource(const std::string& serverUrl, const std::optional<std::string>& resourceMetadataUrl) {
        auto server = m_urls.parse(serverUrl);
        if (!server) {
            return std::unexpected(Error{"oauth_discovery", server.error().message});
        }
        std::string url = resourceMetadataUrl ? *resourceMetadataUrl : m_urls.origin(*server) + "/.well-known/oauth-protected-resource" + pathSuffix(server->path);
        auto response = fetch(url);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!resourceMetadataUrl && server->path != "/" && miss(response->status)) {
            response = fetch(m_urls.origin(*server) + "/.well-known/oauth-protected-resource");
            if (!response) {
                return std::unexpected(response.error());
            }
        }
        if (response->status < 200 || response->status >= 300) {
            return Json();
        }
        const Json document = Json::parse(response->body, nullptr, false);
        if (!document.is_object() || !document.contains("resource") || !document["resource"].is_string() || !m_urls.parse(document["resource"].get<std::string>())) {
            return Json();
        }
        return document;
    }

    /** The authorization server's metadata from the first discovery URL that has it, null when none does. */
    Result<Json> authorizationServerMetadata(const std::string& authorizationServerUrl) {
        auto issuer = m_urls.parse(authorizationServerUrl);
        if (!issuer) {
            return std::unexpected(Error{"oauth_discovery", issuer.error().message});
        }
        const std::string path = pathSuffix(issuer->path);
        std::vector<std::string> candidates{m_urls.origin(*issuer) + "/.well-known/oauth-authorization-server" + path,
                                            m_urls.origin(*issuer) + "/.well-known/openid-configuration" + path};
        if (!path.empty()) {
            candidates.push_back(m_urls.origin(*issuer) + path + "/.well-known/openid-configuration");
        }
        for (const std::string& url : candidates) {
            auto response = fetch(url);
            if (!response) {
                return std::unexpected(response.error());
            }
            if (response->status < 200 || response->status >= 300) {
                if (miss(response->status)) {
                    continue;
                }
                return std::unexpected(Error{"oauth_discovery", "HTTP " + std::to_string(response->status) + " loading authorization server metadata from " + url});
            }
            const Json document = Json::parse(response->body, nullptr, false);
            auto metadata = validMetadata(document);
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            if (trimSlash(document["issuer"].get<std::string>()) != trimSlash(authorizationServerUrl)) {
                return std::unexpected(Error{"oauth_discovery", "OAuth issuer mismatch: expected \"" + authorizationServerUrl + "\", received \"" + document["issuer"].get<std::string>() + "\""});
            }
            return document;
        }
        return Json();
    }

    Result<void> validMetadata(const Json& document) const {
        const auto text = [&](const char* key) { return document.is_object() && document.contains(key) && document[key].is_string() && m_urls.parse(document[key].get<std::string>()).has_value(); };
        if (!text("issuer") || !text("authorization_endpoint") || !text("token_endpoint") || !document.contains("response_types_supported") || !document["response_types_supported"].is_array()) {
            return std::unexpected(Error{"oauth_discovery", "Invalid authorization server metadata"});
        }
        return {};
    }

    /** The resource to request tokens for: the protected resource's identifier when it covers the server URL. */
    Result<std::optional<std::string>> selectResource(const std::string& serverUrl, const Json& metadata) const {
        if (!metadata.is_object() || !metadata.contains("resource")) {
            return std::optional<std::string>();
        }
        auto requested = m_urls.parse(serverUrl);
        auto configured = m_urls.parse(metadata["resource"].get<std::string>());
        if (!requested || !configured) {
            return std::unexpected(Error{"oauth_discovery", "Invalid protected resource URL"});
        }
        requested->fragment.clear();
        const auto slashed = [](const std::string& path) { return path.ends_with("/") ? path : path + "/"; };
        if (m_urls.origin(*requested) != m_urls.origin(*configured) || !slashed(requested->path).starts_with(slashed(configured->path))) {
            return std::unexpected(Error{"oauth_discovery", "Protected resource " + metadata["resource"].get<std::string>() + " does not match MCP server " + m_urls.print(*requested)});
        }
        return std::optional<std::string>(metadata["resource"].get<std::string>());
    }

    /** The configured client, else the registered one; null when there is none. */
    Json clientOf(const Json& state, const Json& settings) const {
        const std::string configured = settings.value("clientId", std::string());
        if (!configured.empty()) {
            Json client = Json{{"client_id", configured}};
            if (settings.contains("clientSecret") && settings["clientSecret"].is_string() && !settings["clientSecret"].get<std::string>().empty()) {
                client["client_secret"] = settings["clientSecret"];
            }
            return client;
        }
        const Json stored = state.value("clientInformation", Json());
        return stored.is_object() && stored.contains("client_id") && stored["client_id"].is_string() ? stored : Json();
    }

    Result<Json> tokenRequest(const Json& discovered, const Json& client, const std::optional<std::string>& resource, const std::vector<std::pair<std::string, std::string>>& grant) {
        const Json metadata = discovered.value("authorizationServerMetadata", Json());
        std::string endpoint;
        if (metadata.is_object() && metadata.contains("token_endpoint")) {
            endpoint = metadata["token_endpoint"].get<std::string>();
        } else {
            auto server = m_urls.parse(discovered["authorizationServerUrl"].get<std::string>());
            if (!server) {
                return std::unexpected(Error{"oauth_discovery", server.error().message});
            }
            endpoint = m_urls.origin(*server) + "/token";
        }
        auto url = m_urls.parse(endpoint);
        if (!url) {
            return std::unexpected(Error{"oauth_discovery", url.error().message});
        }
        if (auto secure = secureEndpoint(*url); !secure) {
            return std::unexpected(secure.error());
        }
        std::vector<std::pair<std::string, std::string>> params = grant;
        if (resource) {
            params.emplace_back("resource", *resource);
        }
        HttpRequest request;
        request.method = "POST";
        request.url = endpoint;
        request.headers = {{"Accept", "application/json"}, {"content-type", "application/x-www-form-urlencoded"}};
        request.timeout = std::chrono::milliseconds(15000);
        request.signal = m_signal;
        if (auto authenticated = authenticate(request, params, client, metadata); !authenticated) {
            return std::unexpected(authenticated.error());
        }
        request.body = formEncode(params);
        auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(response.error());
        }
        return tokensOf(*response);
    }

    Result<void> authenticate(HttpRequest& request, std::vector<std::pair<std::string, std::string>>& params, const Json& client, const Json& metadata) const {
        const std::string method = authMethod(client, metadata);
        const std::string id = client["client_id"].get<std::string>();
        const std::string secret = client.contains("client_secret") && client["client_secret"].is_string() ? client["client_secret"].get<std::string>() : std::string();
        if (method == "client_secret_basic") {
            if (secret.empty()) {
                return std::unexpected(Error{"oauth_client", "client_secret_basic requires a client secret"});
            }
            m_headers.set(request.headers, "Authorization", "Basic " + m_base64.encode(id + ":" + secret));
            return {};
        }
        params.emplace_back("client_id", id);
        if (method == "client_secret_post" && !secret.empty()) {
            params.emplace_back("client_secret", secret);
        }
        return {};
    }

    std::string authMethod(const Json& client, const Json& metadata) const {
        std::vector<std::string> supported;
        if (metadata.is_object() && metadata.contains("token_endpoint_auth_methods_supported") && metadata["token_endpoint_auth_methods_supported"].is_array()) {
            for (const Json& method : metadata["token_endpoint_auth_methods_supported"]) {
                if (method.is_string()) {
                    supported.push_back(method.get<std::string>());
                }
            }
        }
        const auto has = [&](const std::string& method) { return std::ranges::find(supported, method) != supported.end(); };
        const bool secret = client.contains("client_secret") && client["client_secret"].is_string() && !client["client_secret"].get<std::string>().empty();
        const std::string hinted = client.value("token_endpoint_auth_method", std::string());
        if ((hinted == "client_secret_basic" || hinted == "client_secret_post" || hinted == "none") && (supported.empty() || has(hinted))) {
            return hinted;
        }
        if (supported.empty()) {
            return secret ? "client_secret_basic" : "none";
        }
        if (secret && has("client_secret_basic")) {
            return "client_secret_basic";
        }
        if (secret && has("client_secret_post")) {
            return "client_secret_post";
        }
        if (has("none")) {
            return "none";
        }
        return secret ? "client_secret_post" : "none";
    }

    /** The token response; servers report OAuth errors with any status, so the body is checked before the status. */
    Result<Json> tokensOf(const HttpResponse& response) const {
        const Json body = Json::parse(response.body, nullptr, false);
        if (body.is_object() && body.contains("error") && body["error"].is_string()) {
            const std::string description = body.value("error_description", std::string());
            return std::unexpected(Error{"oauth:" + body["error"].get<std::string>(), description.empty() ? body["error"].get<std::string>() : description});
        }
        if (response.status < 200 || response.status >= 300) {
            return std::unexpected(Error{"oauth:server_error", "HTTP " + std::to_string(response.status) + ": " + response.body});
        }
        const auto text = [&](const char* key) { return body.is_object() && body.contains(key) && body[key].is_string() && !body[key].get<std::string>().empty(); };
        if (!text("access_token") || !text("token_type")) {
            return std::unexpected(Error{"oauth:invalid_response", "Invalid OAuth token response"});
        }
        Json tokens = Json{{"access_token", body["access_token"]}, {"token_type", body["token_type"]}};
        if (body.contains("expires_in") && !body["expires_in"].is_null() && body["expires_in"] != "") {
            const double expires = body["expires_in"].is_number() ? body["expires_in"].get<double>() : std::strtod(body["expires_in"].dump().c_str(), nullptr);
            if (!std::isfinite(expires)) {
                return std::unexpected(Error{"oauth:invalid_response", "Invalid expires_in"});
            }
            tokens["expires_in"] = body["expires_in"].is_number() ? body["expires_in"] : Json(expires);
        }
        for (const char* key : {"scope", "refresh_token", "id_token"}) {
            if (text(key)) {
                tokens[key] = body[key];
            }
        }
        return tokens;
    }

    /** `invalid_client`/`invalid_grant` drop what the server rejected and ask for a new sign-in; other errors pass through. */
    Result<Json> failed(const Error& error) const {
        if (error.code == "oauth:invalid_client" || error.code == "oauth:unauthorized_client" || error.code == "oauth:invalid_grant") {
            return std::unexpected(signIn());
        }
        return std::unexpected(error);
    }

    Error signIn() const {
        return Error{"auth_required", "MCP OAuth authorization requires user interaction"};
    }

    Result<void> secureEndpoint(const ParsedUrl& url) const {
        if (url.scheme != "https" && !m_urls.loopback(url)) {
            return std::unexpected(Error{"oauth_insecure", "Refusing to send OAuth credentials to non-HTTPS endpoint " + m_urls.print(url)});
        }
        return {};
    }

    Result<HttpResponse> fetch(const std::string& url) {
        HttpRequest request;
        request.url = url;
        request.headers = {{"Accept", "application/json"}, {"MCP-Protocol-Version", std::string(kProtocolVersion)}};
        request.timeout = std::chrono::milliseconds(15000);
        request.signal = m_signal;
        return m_http.send(request);
    }

    Result<Json> fetchJson(const std::string& url) {
        auto response = fetch(url);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"oauth_discovery", "HTTP " + std::to_string(response->status) + " loading authorization server metadata from " + url});
        }
        const Json document = Json::parse(response->body, nullptr, false);
        if (document.is_discarded()) {
            return std::unexpected(Error{"oauth_discovery", "Invalid authorization server metadata"});
        }
        return document;
    }

    /** 4xx and 502 mean "not here", so discovery tries the next candidate URL. */
    bool miss(int status) const {
        return (status >= 400 && status < 500) || status == 502;
    }

    std::string pathSuffix(const std::string& path) const {
        return path.ends_with("/") ? path.substr(0, path.size() - 1) : path;
    }

    std::string trimSlash(const std::string& value) const {
        return value.ends_with("/") ? value.substr(0, value.size() - 1) : value;
    }

    std::string formEncode(const std::vector<std::pair<std::string, std::string>>& params) const {
        std::string out;
        for (const auto& [name, value] : params) {
            out += (out.empty() ? "" : "&") + escape(name) + "=" + escape(value);
        }
        return out;
    }

    std::string escape(const std::string& value) const {
        std::string out;
        for (const char c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '*') {
                out.push_back(c);
            } else if (c == ' ') {
                out.push_back('+');
            } else {
                out += std::format("%{:02X}", byte);
            }
        }
        return out;
    }

    IHttpClient& m_http;
    const IClock& m_clock;
    const IBase64Codec& m_base64;
    UrlParser m_urls;
    HeaderMerger m_headers;
    std::shared_ptr<AbortSignal> m_signal;
};
