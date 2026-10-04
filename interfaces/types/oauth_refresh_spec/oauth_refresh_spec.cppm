export module pi.types.oauth_refresh_spec;

import std;

/** How one provider's OAuth refresh token is exchanged and how its access token is used. */
export struct OauthRefreshSpec {
    std::string providerId;
    std::string name;
    bool isSubscription = true;
    std::string tokenUrl;
    /** JSON request body instead of application/x-www-form-urlencoded. */
    bool jsonBody = false;
    /** Fixed client id; empty reads "clientId" from the stored credential instead. */
    std::string clientId;
    std::string missingClientIdMessage;
    std::map<std::string, std::string> extraParams;
    std::int64_t expiryMarginMs = 0;
    /** The server may omit refresh_token when it is not rotated. */
    bool keepRefreshWhenMissing = false;
    /** The grant's scope must include this one; the scopes are stored with the credential. */
    std::string requiredScope;
    /** Stores the `scope` string of the token response with the credential (Radius). */
    bool keepScope = false;
    /** Stores the ChatGPT account id from the access token as "accountId". */
    bool accountIdFromJwt = false;
    /** Authenticate with an Authorization: Bearer header instead of an API key. */
    bool bearerHeader = false;
    /** The credential is a long-lived key; refresh returns it unchanged. */
    bool passthrough = false;
};
