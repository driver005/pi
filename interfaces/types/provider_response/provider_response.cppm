export module pi.types.provider_response;

import std;
export import pi.types.http_headers;

/** HTTP status and headers reported to StreamOptions::onResponse. */
export struct ProviderResponse {
    int status = 0;
    HttpHeaders headers;
};
