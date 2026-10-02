#pragma once

#include "interfaces/types/http_request/http_request.h"
#include "interfaces/types/http_response/http_response.h"
#include "interfaces/types/result/result.h"

/** Blocking HTTP(S) client with streaming and cancellation. Thread-safe. */
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /**
     * Performs the request. A non-2xx status is a successful Result (inspect `status`);
     * Error covers transport failures, timeouts (code "timeout") and aborts (code "aborted").
     */
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};
