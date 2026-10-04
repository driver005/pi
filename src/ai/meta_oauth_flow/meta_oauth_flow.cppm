export module pi.ai.meta_oauth_flow;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_http_client;
export import pi.provider.i_oauth_flow;
export import pi.types.json;

/**
 * Meta Model API: the identity token of the device sign-in (stored as `refresh`) is not accepted for inference; it is traded for
 * a Model API key at the Muse Code key-mint endpoint, which lives about a day and is stored as `access`. The identity token cannot
 * be renewed, so refreshing is minting again, and a 401 or 403 means the session is dead and the person has to sign in again.
 * Port of auth/oauth/meta.ts (the device flow itself is OauthDeviceLogin over ProviderLoginSpecs).
 */
export class MetaOauthFlow : public IOauthFlow {
public:
    MetaOauthFlow(IHttpClient& http, const IClock& clock)
        : m_http(http),
          m_clock(clock) {}

    std::string providerId() const override {
        return "meta";
    }

    std::string name() const override {
        return "Meta (Muse subscription)";
    }

    bool isSubscription() const override {
        return true;
    }

    /** Mints a key from `credential.refresh`, the identity token. */
    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>& signal) override {
        HttpRequest request;
        request.method = "POST";
        request.url = "https://api.meta.ai/muse-code/key";
        request.headers = {{"Accept", "application/json"},
                           {"Authorization", "Bearer " + credential.refresh},
                           {"Content-Type", "application/json"},
                           {"x-api-version", "1.0.0"}};
        request.body = "{}";
        request.timeout = std::chrono::seconds(30);
        request.signal = signal;
        const auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(Error{"oauth", "Meta API key mint request failed: " + response.error().message});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (response->status == 401 || response->status == 403) {
            return std::unexpected(Error{"oauth", "Meta session expired (status " + std::to_string(response->status) + "). Run `pi auth login meta` to sign in again." + detail(body)});
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"oauth", "Meta API key mint failed with status " + std::to_string(response->status) + detail(body)});
        }
        const std::string key = text(body, "api_key");
        if (key.empty()) {
            const std::string action = httpUrl(text(body, "action_url"));
            return std::unexpected(Error{"oauth", "Meta did not issue an API key." + (action.empty() ? std::string() : " Complete setup at " + action)});
        }
        Credential out = credential;
        out.type = CredentialType::OAuth;
        out.access = key;
        out.expires = static_cast<double>(m_clock.nowMs() + kKeyLifetimeMs);
        return out;
    }

    ModelAuth toAuth(const Credential& credential) const override {
        ModelAuth auth;
        auth.apiKey = credential.access;
        return auth;
    }

private:
    static constexpr std::int64_t kKeyLifetimeMs = 24LL * 60 * 60 * 1000;

    std::string text(const Json& body, const std::string& key) const {
        return body.is_object() && body.contains(key) && body[key].is_string() ? body[key].get<std::string>() : "";
    }

    /** ": <description>" from the first of the usual error fields the response has. */
    std::string detail(const Json& body) const {
        for (const char* key : {"error_description", "detail", "message", "error"}) {
            std::string value = text(body, key);
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                const auto last = value.find_last_not_of(" \t\r\n");
                return ": " + value.substr(first, last - first + 1);
            }
        }
        return "";
    }

    /** Only http(s) URLs are shown to the person. */
    std::string httpUrl(const std::string& value) const {
        return value.starts_with("https://") || value.starts_with("http://") ? value : "";
    }

    IHttpClient& m_http;
    const IClock& m_clock;
};
