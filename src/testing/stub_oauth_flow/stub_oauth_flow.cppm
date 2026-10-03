export module pi.testing.stub_oauth_flow;

import std;
export import pi.provider.i_oauth_flow;

/** IOauthFlow whose refresh succeeds with a fresh token (or fails on demand) and counts calls. */
export class StubOauthFlow : public IOauthFlow {
public:
    explicit StubOauthFlow(std::string providerId = "anthropic")
        : m_providerId(std::move(providerId)) {}

    void failRefreshWith(std::string message) {
        m_failure = std::move(message);
    }

    int refreshCount() const {
        return m_refreshes;
    }

    std::string providerId() const override {
        return m_providerId;
    }

    std::string name() const override {
        return m_providerId;
    }

    bool isSubscription() const override {
        return true;
    }

    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>&) override {
        ++m_refreshes;
        if (m_failure) {
            return std::unexpected(Error{"oauth", *m_failure});
        }
        Credential next = credential;
        next.access = "fresh-access";
        next.expires = 9e15;
        return next;
    }

    ModelAuth toAuth(const Credential& credential) const override {
        ModelAuth auth;
        auth.apiKey = credential.access;
        return auth;
    }

private:
    std::string m_providerId;
    std::optional<std::string> m_failure;
    int m_refreshes = 0;
};
