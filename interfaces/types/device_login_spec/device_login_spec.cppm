module;

#include <cstdint>

export module pi.types.device_login_spec;

import std;

/** The device authorization grant (RFC 8628) of one provider: the endpoints, the client and what the responses must carry. */
export struct DeviceLoginSpec {
    std::string name;
    std::string deviceUrl;
    std::string tokenUrl;
    std::string clientId;
    std::string scope;
    /** Fixed extra form parameters of the device authorization request. */
    std::vector<std::pair<std::string, std::string>> deviceParams;
    /** Shows `verification_uri_complete` when the server sent one. */
    bool preferCompleteUri = false;
    /** The response must carry `verification_uri_complete`. */
    bool requireCompleteUri = false;
    /** The verification URIs must be https (else http is accepted too). */
    bool httpsOnly = false;
    /** Used when the response has no `interval` / `expires_in`; an expiry of 0 means the response must carry one. */
    std::int64_t defaultIntervalSeconds = 5;
    std::int64_t defaultExpiresSeconds = 0;
    bool waitBeforeFirstPoll = true;
    /** A 5xx answer of the token endpoint fails the sign-in instead of being read as a pending one. */
    bool serverErrorFails = false;
    /** Extra headers of both requests (GitHub wants a user agent). */
    std::vector<std::pair<std::string, std::string>> headers;
    /** Form field carrying the device code in the token request. */
    std::string deviceCodeField = "device_code";
};
