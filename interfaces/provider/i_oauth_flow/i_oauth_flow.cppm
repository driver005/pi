export module pi.provider.i_oauth_flow;

import std;
export import pi.support.abort_signal;
export import pi.types.credential;
export import pi.types.model_auth;
export import pi.types.result;

/** Token refresh and request-auth derivation for one provider's OAuth credentials. */
export class IOauthFlow {
public:
    virtual ~IOauthFlow() = default;

    virtual std::string providerId() const = 0;
    virtual std::string name() const = 0;
    virtual bool isSubscription() const = 0;

    /** Exchanges the refresh token. Called under the credential store's lock. */
    virtual Result<Credential> refresh(const Credential& credential,
                                       const std::shared_ptr<AbortSignal>& signal) = 0;

    /** Side-effect free: the API key (and base URL or headers) a valid credential grants. */
    virtual ModelAuth toAuth(const Credential& credential) const = 0;
};
