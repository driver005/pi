module;

#include <cstdint>

export module pi.ai.oauth_refresh_flow;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.provider.i_oauth_flow;
export import pi.support.oauth_token_mapper;
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
        : m_spec(spec),
          m_http(http),
          m_mapper(std::move(spec), clock, base64) {}

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
        const auto clientId = m_mapper.clientIdFor(credential);
        if (!clientId) {
            return std::unexpected(clientId.error());
        }
        const auto response = m_http.send(m_mapper.refreshRequest(credential, *clientId, signal));
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
        return m_mapper.credentialFrom(body, credential, *clientId);
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
    OauthRefreshSpec m_spec;
    IHttpClient& m_http;
    OauthTokenMapper m_mapper;
};
