export module pi.provider.i_access_token_source;

import std;
export import pi.types.result;

/** Supplies OAuth bearer tokens for providers that authenticate with a cloud identity. */
export class IAccessTokenSource {
public:
    virtual ~IAccessTokenSource() = default;

    /**
     * A valid access token, refreshed when needed. env holds provider-scoped environment values
     * that take precedence over the process environment (credential file location, ...).
     */
    virtual Result<std::string> token(const std::map<std::string, std::string>& env) = 0;
};
