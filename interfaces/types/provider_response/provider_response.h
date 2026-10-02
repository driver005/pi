#pragma once

#include "interfaces/types/http_headers/http_headers.h"

/** HTTP status and headers reported to StreamOptions::onResponse. */
struct ProviderResponse {
    int status = 0;
    HttpHeaders headers;
};
