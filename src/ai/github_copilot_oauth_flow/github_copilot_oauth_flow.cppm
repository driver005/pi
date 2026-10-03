export module pi.ai.github_copilot_oauth_flow;

import std;
export import pi.platform.i_http_client;
export import pi.provider.i_oauth_flow;
export import pi.types.json;

/**
 * GitHub Copilot: the long-lived GitHub token (stored as `refresh`) is exchanged for a short-lived
 * Copilot token at api.<domain>/copilot_internal/v2/token. The API base URL comes from the
 * token's proxy-ep claim. Port of the refresh and toAuth parts of auth/oauth/github-copilot.ts;
 * the model availability fetch that follows a refresh in TypeScript is not ported (the stored
 * availableModelIds stay as they are).
 */
export class GithubCopilotOauthFlow : public IOauthFlow {
public:
    explicit GithubCopilotOauthFlow(IHttpClient& http);

    std::string providerId() const override;
    std::string name() const override;
    bool isSubscription() const override;
    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>& signal) override;
    ModelAuth toAuth(const Credential& credential) const override;

private:
    std::optional<std::string> enterpriseDomain(const Credential& credential) const;
    std::string baseUrl(const std::string& token, const std::optional<std::string>& domain) const;

    IHttpClient& m_http;
};

GithubCopilotOauthFlow::GithubCopilotOauthFlow(IHttpClient& http) : m_http(http) {}

std::string GithubCopilotOauthFlow::providerId() const {
    return "github-copilot";
}

std::string GithubCopilotOauthFlow::name() const {
    return "GitHub Copilot";
}

bool GithubCopilotOauthFlow::isSubscription() const {
    return true;
}

std::optional<std::string> GithubCopilotOauthFlow::enterpriseDomain(const Credential& credential) const {
    if (!credential.extra.is_object() || !credential.extra.contains("enterpriseUrl") ||
        !credential.extra["enterpriseUrl"].is_string()) {
        return std::nullopt;
    }
    std::string text = credential.extra["enterpriseUrl"].get<std::string>();
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
    if (const auto scheme = text.find("://"); scheme != std::string::npos) {
        text = text.substr(scheme + 3);
    }
    text = text.substr(0, text.find_first_of("/:?#"));
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
}

std::string GithubCopilotOauthFlow::baseUrl(const std::string& token, const std::optional<std::string>& domain) const {
    const auto marker = token.find("proxy-ep=");
    if (marker != std::string::npos) {
        const auto start = marker + 9;
        std::string host = token.substr(start, token.find(';', start) - start);
        if (host.starts_with("proxy.")) {
            host = "api." + host.substr(6);
        }
        return "https://" + host;
    }
    return domain ? "https://copilot-api." + *domain : "https://api.individual.githubcopilot.com";
}

Result<Credential> GithubCopilotOauthFlow::refresh(const Credential& credential,
                                                   const std::shared_ptr<AbortSignal>& signal) {
    const auto domain = enterpriseDomain(credential);
    HttpRequest request;
    request.url = "https://api." + domain.value_or("github.com") + "/copilot_internal/v2/token";
    request.headers = {{"Accept", "application/json"},
                       {"Authorization", "Bearer " + credential.refresh},
                       {"User-Agent", "GitHubCopilotChat/0.35.0"},
                       {"Editor-Version", "vscode/1.107.0"},
                       {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
                       {"Copilot-Integration-Id", "vscode-chat"}};
    request.timeout = std::chrono::seconds(30);
    request.signal = signal;
    const auto response = m_http.send(request);
    if (!response) {
        return std::unexpected(Error{"oauth", "Copilot token request failed: " + response.error().message});
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(Error{"oauth", "Copilot token request failed (" + std::to_string(response->status) +
                                                  "): " + response->body.substr(0, 500)});
    }
    const Json body = Json::parse(response->body, nullptr, false);
    if (!body.is_object()) {
        return std::unexpected(Error{"oauth", "Invalid Copilot token response"});
    }
    if (!body.contains("token") || !body["token"].is_string() || !body.contains("expires_at") ||
        !body["expires_at"].is_number()) {
        return std::unexpected(Error{"oauth", "Invalid Copilot token response fields"});
    }
    Credential out = credential;
    out.type = CredentialType::OAuth;
    out.access = body["token"].get<std::string>();
    out.expires = body["expires_at"].get<double>() * 1000.0 - 5.0 * 60.0 * 1000.0;
    return out;
}

ModelAuth GithubCopilotOauthFlow::toAuth(const Credential& credential) const {
    ModelAuth auth;
    auth.apiKey = credential.access;
    auth.baseUrl = baseUrl(credential.access, enterpriseDomain(credential));
    return auth;
}
