export module pi.testing.stub_oauth_flow;

import std;
export import pi.provider.i_oauth_flow;

/** IOauthFlow whose refresh succeeds with a fresh token (or fails on demand) and counts calls. */
export class StubOauthFlow : public IOauthFlow {
public:
    explicit StubOauthFlow(std::string providerId = "anthropic");

    void failRefreshWith(std::string message);
    int refreshCount() const;

    std::string providerId() const override;
    std::string name() const override;
    bool isSubscription() const override;
    Result<Credential> refresh(const Credential& credential,
                               const std::shared_ptr<AbortSignal>& signal) override;
    ModelAuth toAuth(const Credential& credential) const override;

private:
    std::string m_providerId;
    std::optional<std::string> m_failure;
    int m_refreshes = 0;
};

StubOauthFlow::StubOauthFlow(std::string providerId) : m_providerId(std::move(providerId)) {}

void StubOauthFlow::failRefreshWith(std::string message) {
    m_failure = std::move(message);
}

int StubOauthFlow::refreshCount() const {
    return m_refreshes;
}

std::string StubOauthFlow::providerId() const {
    return m_providerId;
}

std::string StubOauthFlow::name() const {
    return m_providerId;
}

bool StubOauthFlow::isSubscription() const {
    return true;
}

Result<Credential> StubOauthFlow::refresh(const Credential& credential,
                                          const std::shared_ptr<AbortSignal>&) {
    ++m_refreshes;
    if (m_failure) {
        return std::unexpected(Error{"oauth", *m_failure});
    }
    Credential next = credential;
    next.access = "fresh-access";
    next.expires = 9e15;
    return next;
}

ModelAuth StubOauthFlow::toAuth(const Credential& credential) const {
    ModelAuth auth;
    auth.apiKey = credential.access;
    return auth;
}
