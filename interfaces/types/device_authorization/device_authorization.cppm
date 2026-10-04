module;

#include <cstdint>

export module pi.types.device_authorization;

import std;

/** A provider's answer to a device authorization request. */
export struct DeviceAuthorization {
    std::string deviceCode;
    std::string userCode;
    std::string uri;
    /** `verification_uri_complete`: the URI with the user code filled in; empty when the server sent none. */
    std::string completeUri;
    std::optional<std::int64_t> intervalSeconds;
    std::optional<std::int64_t> expiresInSeconds;
};
