export module pi.platform.i_http_client;

import std;
export import pi.types.http_request;
export import pi.types.http_response;
export import pi.types.result;

/** Blocking HTTP(S) client with streaming and cancellation. Thread-safe. */
export class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /**
     * Performs the request. A non-2xx status is a successful Result (inspect `status`);
     * Error covers transport failures, timeouts (code "timeout") and aborts (code "aborted").
     */
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};
