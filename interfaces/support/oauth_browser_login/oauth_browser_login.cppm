module;

#include <cstdint>

export module pi.support.oauth_browser_login;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_callback_server;
export import pi.platform.i_crypto;
export import pi.platform.i_http_client;
export import pi.support.oauth_token_mapper;
export import pi.support.url_parser;
export import pi.types.browser_login_spec;
export import pi.types.login_interaction;

/**
 * The OAuth authorization code flow with PKCE (RFC 7636) of a provider's subscription or key sign-in: sends the user to the
 * authorization URL (shown through the interaction), receives the redirect at a loopback callback server (or a pasted code or
 * redirect URL when the browser cannot reach it), exchanges the code and returns the credential in the shape the provider's
 * refresh keeps. The provider's endpoints and quirks are in the BrowserLoginSpec and its OauthRefreshSpec (through the
 * OauthTokenMapper). Port of the browser flows of anthropic.ts, openai-codex.ts, openai-chatgpt.ts and openrouter.ts in
 * packages/ai/src/auth/oauth.
 *
 * Errors: "oauth:<code>" for what the authorization server reported, "oauth" for failed exchanges and mismatching states,
 * "callback_server" when a required port is taken, plus the callback server's "timeout" and "aborted".
 */
export class OauthBrowserLogin {
public:
    using CallbackFactory = std::function<std::unique_ptr<ICallbackServer>()>;
    using Param = std::pair<std::string, std::string>;

    static constexpr std::chrono::milliseconds kDefaultTimeout{5 * 60 * 1000};

    OauthBrowserLogin(IHttpClient& http, const ICrypto& crypto, const IBase64Codec& base64, CallbackFactory callbacks)
        : m_http(http),
          m_crypto(crypto),
          m_base64(base64),
          m_callbacks(std::move(callbacks)) {}

    /**
     * Signs in. `extraParams` are added to the authorization URL (values only known at sign-in time, such as nonces);
     * `copyCode` selects the variant where the provider shows the code and the user pastes it.
     */
    Result<Credential> login(const BrowserLoginSpec& spec, const OauthTokenMapper& mapper, const std::vector<Param>& extraParams, bool copyCode, const LoginInteraction& interaction, std::chrono::milliseconds timeout = kDefaultTimeout) {
        const std::string verifier = m_base64.encodeUrl(m_crypto.randomBytes(32));
        const std::string challenge = m_base64.encodeUrl(m_crypto.sha256(verifier));
        const std::string state = spec.noState ? "" : spec.stateIsVerifier ? verifier : hex(m_crypto.randomBytes(16));
        const std::string path = spec.randomPath ? spec.path + "/" + hex(m_crypto.randomBytes(16)) : spec.path;

        std::unique_ptr<ICallbackServer> callback;
        int port = spec.port;
        if (!copyCode && !interaction.manualOnly) {
            callback = m_callbacks();
            auto bound = callback->listen("127.0.0.1", spec.port, spec.port != 0);
            if (!bound) {
                callback.reset();
                if (!spec.pasteWhenPortIsTaken || !interaction.manualCode) {
                    return std::unexpected(Error{"callback_server", bound.error().message + (spec.port != 0 ? " (port " + std::to_string(spec.port) + " is in use, probably by an unfinished sign-in in another session)" : "")});
                }
            } else {
                port = *bound;
            }
        }
        if (!copyCode && !callback && !interaction.manualCode) {
            return std::unexpected(Error{"oauth", "Cannot ask for the authorization code: no input is available"});
        }
        const std::string redirect = copyCode ? spec.copyCodeRedirectUri : "http://" + spec.redirectHost + ":" + std::to_string(port) + path;
        auto done = authorize(spec, mapper, extraParams, interaction, timeout, callback.get(), verifier, challenge, state, path, redirect);
        if (callback) {
            callback->close();
        }
        return done;
    }

private:
    Result<Credential> authorize(const BrowserLoginSpec& spec, const OauthTokenMapper& mapper, const std::vector<Param>& extraParams, const LoginInteraction& interaction, std::chrono::milliseconds timeout, ICallbackServer* callback, const std::string& verifier, const std::string& challenge,
                                 const std::string& state, const std::string& path, const std::string& redirect) {
        if (interaction.authUrl) {
            interaction.authUrl(authorizationUrl(spec, extraParams, challenge, state, redirect),
                                callback != nullptr ? "Complete login in your browser. If the browser is on another machine, paste the final redirect URL when asked."
                                                    : "Complete login in your browser, then paste the authorization code or the final redirect URL.");
        }
        std::string code;
        std::string clientId = spec.clientId;
        std::string exchangedState = state;
        if (callback != nullptr) {
            auto response = callback->waitForCallback({path}, state, timeout, interaction.signal);
            if (!response) {
                return std::unexpected(response.error());
            }
            if (response->contains("error")) {
                const std::string error = (*response)["error"].get<std::string>();
                return std::unexpected(Error{"oauth:" + error, spec.providerName + " authorization failed: " + response->value("error_description", error)});
            }
            code = response->value("code", std::string());
            if (spec.dynamicClientId) {
                clientId = response->value("client_id", std::string());
                if (clientId.empty()) {
                    return std::unexpected(Error{"oauth", spec.providerName + " OAuth registration callback did not contain an issued client ID"});
                }
            }
        } else {
            auto pasted = interaction.manualCode("Complete login in your browser, or paste the authorization code / redirect URL here:");
            if (!pasted) {
                return std::unexpected(Error{"login_cancelled", "Login cancelled"});
            }
            const auto parsed = parseInput(*pasted);
            if (!parsed.second.empty() && !state.empty() && parsed.second != state) {
                return std::unexpected(Error{"oauth", "OAuth state mismatch"});
            }
            code = parsed.first;
            exchangedState = parsed.second.empty() ? state : parsed.second;
        }
        if (code.empty()) {
            return std::unexpected(Error{"oauth", "Missing authorization code"});
        }
        if (interaction.progress) {
            interaction.progress(spec.exchangesForKey ? "Exchanging authorization code for an API key..." : "Exchanging authorization code for tokens...");
        }
        return exchange(spec, mapper, code, verifier, clientId, redirect, exchangedState, interaction.signal);
    }

    std::string authorizationUrl(const BrowserLoginSpec& spec, const std::vector<Param>& extraParams, const std::string& challenge, const std::string& state, const std::string& redirect) const {
        std::vector<Param> params = spec.authorizeParams;
        params.insert(params.end(), extraParams.begin(), extraParams.end());
        if (spec.sendClientId) {
            params.emplace_back("response_type", "code");
            params.emplace_back("client_id", spec.clientId);
        }
        params.emplace_back(spec.redirectParam, redirect);
        if (spec.sendClientId && !spec.scope.empty()) {
            params.emplace_back("scope", spec.scope);
        }
        params.emplace_back("code_challenge", challenge);
        params.emplace_back("code_challenge_method", "S256");
        if (!state.empty()) {
            params.emplace_back("state", state);
        }
        std::string url = spec.authorizeUrl + (spec.authorizeUrl.find('?') == std::string::npos ? "?" : "&");
        for (std::size_t i = 0; i < params.size(); ++i) {
            url += (i == 0 ? "" : "&") + m_urls.encode(params[i].first) + "=" + m_urls.encode(params[i].second);
        }
        return url;
    }

    Result<Credential> exchange(const BrowserLoginSpec& spec, const OauthTokenMapper& mapper, const std::string& code, const std::string& verifier, const std::string& clientId, const std::string& redirect, const std::string& state, const std::shared_ptr<AbortSignal>& signal) {
        const std::string name = mapper.spec().name;
        HttpRequest request;
        if (spec.exchangesForKey) {
            request.method = "POST";
            request.url = spec.exchangeUrl;
            request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
            request.body = Json{{"code", code}, {"code_verifier", verifier}, {"code_challenge_method", "S256"}}.dump();
            request.timeout = std::chrono::seconds(30);
            request.signal = signal;
        } else {
            std::map<std::string, std::string> params{{"grant_type", "authorization_code"}, {"client_id", clientId}, {"code", code}, {"code_verifier", verifier}, {"redirect_uri", redirect}};
            if (spec.stateInTokenRequest) {
                params["state"] = state;
            }
            request = mapper.tokenRequest(std::move(params), signal);
        }
        const auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(Error{"oauth", name + " token exchange request failed: " + response.error().message});
        }
        if (response->status < 200 || response->status >= 300) {
            const std::string body = response->body.size() > 500 ? response->body.substr(0, 500) : response->body;
            return std::unexpected(Error{"oauth", name + " token exchange failed (" + std::to_string(response->status) + "): " + body});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (!body.is_object()) {
            return std::unexpected(Error{"oauth", name + " token exchange returned invalid JSON"});
        }
        if (spec.exchangesForKey) {
            if (!body.contains("key") || !body["key"].is_string() || body["key"].get<std::string>().empty()) {
                return std::unexpected(Error{"oauth", name + " response carries no \"key\""});
            }
            Credential key;
            key.type = CredentialType::OAuth;
            key.access = body["key"].get<std::string>();
            key.expires = 9007199254740991.0;
            return key;
        }
        if (spec.requireIdToken && (!body.contains("id_token") || !body["id_token"].is_string() || body["id_token"].get<std::string>().empty())) {
            return std::unexpected(Error{"oauth", name + " token response did not contain an ID token"});
        }
        return mapper.credentialFrom(body, Credential{}, clientId);
    }

    /** `{code, state}` of a pasted redirect URL, `code#state`, a query string or a bare code. */
    std::pair<std::string, std::string> parseInput(const std::string& input) const {
        const std::size_t first = input.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const std::string value = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
        const std::size_t question = value.find('?');
        const bool isUrl = value.starts_with("http://") || value.starts_with("https://");
        if (isUrl || value.find("code=") != std::string::npos) {
            std::string query = isUrl ? (question == std::string::npos ? std::string() : value.substr(question + 1)) : value;
            if (const std::size_t hash = query.find('#'); hash != std::string::npos) {
                query.resize(hash);
            }
            std::pair<std::string, std::string> out;
            for (const auto& [name, text] : m_urls.parseQuery(query)) {
                if (name == "code") {
                    out.first = text;
                } else if (name == "state") {
                    out.second = text;
                }
            }
            return out;
        }
        if (const std::size_t hash = value.find('#'); hash != std::string::npos) {
            return {value.substr(0, hash), value.substr(hash + 1)};
        }
        return {value, ""};
    }

    std::string hex(const std::string& bytes) const {
        std::string out;
        for (const char byte : bytes) {
            out += std::format("{:02x}", static_cast<unsigned char>(byte));
        }
        return out;
    }

    IHttpClient& m_http;
    const ICrypto& m_crypto;
    const IBase64Codec& m_base64;
    CallbackFactory m_callbacks;
    UrlParser m_urls;
};
