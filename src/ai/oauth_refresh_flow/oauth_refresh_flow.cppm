module;

#include <cstdint>

export module pi.ai.oauth_refresh_flow;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.provider.i_oauth_flow;
export import pi.types.json;
export import pi.types.oauth_refresh_spec;

/**
 * IOauthFlow for providers whose refresh token is exchanged at a token endpoint, described by an
 * OauthRefreshSpec. Port of the refresh and toAuth parts of the TypeScript oauth modules in packages/ai/src/auth for
 * Anthropic, OpenAI Codex, Sign in with ChatGPT, xAI, Kimi Code and OpenRouter; the interactive
 * login belongs to the clients.
 */
export class OauthRefreshFlow : public IOauthFlow {
public:
    OauthRefreshFlow(OauthRefreshSpec spec, IHttpClient& http, const IClock& clock, const IBase64Codec& base64)
        : m_spec(std::move(spec)),
          m_http(http),
          m_clock(clock),
          m_base64(base64) {}

    std::string providerId() const override {
        return m_spec.providerId;
    }

    std::string name() const override {
        return m_spec.name;
    }

    bool isSubscription() const override {
        return m_spec.isSubscription;
    }

    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>& signal) override {
        if (m_spec.passthrough) {
            return credential;
        }
        const auto clientId = clientIdFor(credential);
        if (!clientId) {
            return std::unexpected(clientId.error());
        }
        const auto response = m_http.send(buildRequest(credential, *clientId, signal));
        if (!response) {
            return std::unexpected(Error{"oauth", m_spec.name + " token refresh request failed: " + response.error().message});
        }
        if (response->status < 200 || response->status >= 300) {
            const std::string body = response->body.size() > 500 ? response->body.substr(0, 500) : response->body;
            return std::unexpected(Error{"oauth", m_spec.name + " token refresh failed (" + std::to_string(response->status) +
                                                      "): " + body});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (!body.is_object()) {
            return std::unexpected(Error{"oauth", m_spec.name + " token refresh returned invalid JSON"});
        }
        return credentialFrom(body, credential, *clientId);
    }

    ModelAuth toAuth(const Credential& credential) const override {
        ModelAuth auth;
        if (m_spec.bearerHeader) {
            auth.headers = {{"Authorization", "Bearer " + credential.access}};
        } else {
            auth.apiKey = credential.access;
        }
        return auth;
    }

private:
    Result<std::string> clientIdFor(const Credential& credential) const {
        if (!m_spec.clientId.empty()) {
            return m_spec.clientId;
        }
        const std::string stored = stringIn(credential.extra, "clientId");
        if (stored.find_first_not_of(" \t") == std::string::npos) {
            return std::unexpected(Error{"oauth", m_spec.missingClientIdMessage.empty()
                                                      ? "Stored OAuth credential does not contain a client ID"
                                                      : m_spec.missingClientIdMessage});
        }
        return stored;
    }

    HttpRequest buildRequest(const Credential& credential, const std::string& clientId, const std::shared_ptr<AbortSignal>& signal) const {
        std::map<std::string, std::string> params = m_spec.extraParams;
        params["grant_type"] = "refresh_token";
        params["client_id"] = clientId;
        params["refresh_token"] = credential.refresh;
        HttpRequest request;
        request.method = "POST";
        request.url = m_spec.tokenUrl;
        request.signal = signal;
        request.timeout = std::chrono::seconds(30);
        request.headers = {{"Accept", "application/json"}};
        if (m_spec.jsonBody) {
            request.headers.emplace_back("Content-Type", "application/json");
            request.body = Json(params).dump();
        } else {
            request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
            for (const auto& entry : params) {
                request.body += (request.body.empty() ? "" : "&") + formEncode(entry.first) + "=" + formEncode(entry.second);
            }
        }
        return request;
    }

    Result<Credential> credentialFrom(const Json& body, const Credential& previous, const std::string& clientId) const {
        const std::string access = stringIn(body, "access_token");
        if (access.find_first_not_of(" \t") == std::string::npos) {
            return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid access_token"});
        }
        std::string refresh = stringIn(body, "refresh_token");
        if (refresh.empty()) {
            if (!m_spec.keepRefreshWhenMissing) {
                return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid refresh_token"});
            }
            refresh = previous.refresh;
        }
        double lifetime = 3600;
        if (body.contains("expires_in")) {
            if (!body["expires_in"].is_number() || body["expires_in"].get<double>() <= 0) {
                return std::unexpected(Error{"oauth", m_spec.name + " token response has invalid expires_in"});
            }
            lifetime = body["expires_in"].get<double>();
        }
        Credential out;
        out.type = CredentialType::OAuth;
        out.access = access;
        out.refresh = refresh;
        out.expires = static_cast<double>(m_clock.nowMs()) + lifetime * 1000.0 - static_cast<double>(m_spec.expiryMarginMs);
        out.extra = previous.extra.is_object() ? previous.extra : Json::object();
        if (m_spec.clientId.empty()) {
            out.extra["clientId"] = clientId;
        }
        if (!m_spec.requiredScope.empty()) {
            const std::string scope = stringIn(body, "scope");
            Json scopes = Json::array();
            std::istringstream words(scope);
            for (std::string word; words >> word;) {
                scopes.push_back(word);
            }
            if (std::find(scopes.begin(), scopes.end(), Json(m_spec.requiredScope)) == scopes.end()) {
                return std::unexpected(Error{"oauth", m_spec.name + " grant did not include " + m_spec.requiredScope});
            }
            out.extra["scopes"] = std::move(scopes);
        }
        if (m_spec.accountIdFromJwt) {
            const auto id = accountId(access);
            if (!id) {
                return std::unexpected(Error{"oauth", "Failed to extract accountId from token"});
            }
            out.extra["accountId"] = *id;
        }
        return out;
    }

    std::string formEncode(const std::string& text) const {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(c);
            } else {
                out.push_back('%');
                out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
            }
        }
        return out;
    }

    std::optional<std::string> accountId(const std::string& token) const {
        const auto first = token.find('.');
        const auto second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
        if (second == std::string::npos) {
            return std::nullopt;
        }
        const auto payload = m_base64.decode(token.substr(first + 1, second - first - 1));
        const Json claims = payload ? Json::parse(*payload, nullptr, false) : Json();
        if (claims.is_object() && claims.contains("https://api.openai.com/auth") && claims["https://api.openai.com/auth"].is_object()) {
            const std::string id = stringIn(claims["https://api.openai.com/auth"], "chatgpt_account_id");
            return id.empty() ? std::nullopt : std::optional<std::string>(id);
        }
        return std::nullopt;
    }

    std::string stringIn(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
    }

    OauthRefreshSpec m_spec;
    IHttpClient& m_http;
    const IClock& m_clock;
    const IBase64Codec& m_base64;
};
