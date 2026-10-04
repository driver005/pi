export module pi.types.model_auth;

import std;
export import pi.types.http_headers;

/**
 * Request auth for one model call. Anything that cannot be an API key, header or base URL is
 * provider configuration, not auth.
 */
export struct ModelAuth {
    std::optional<std::string> apiKey;
    HttpHeaders headers;
    std::optional<std::string> baseUrl;
};
