export module pi.platform.i_callback_server;

import std;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.result;

/**
 * A one-shot loopback HTTP server that receives the redirect of an OAuth authorization (the browser calls
 * `http://127.0.0.1:<port>/callback?code=...&state=...`). Made per sign-in; close() ends it.
 */
export class ICallbackServer {
public:
    virtual ~ICallbackServer() = default;

    /**
     * Starts listening on `host` (an address; "localhost" means 127.0.0.1) and port `port` (0: any free one) and returns the
     * port. When the port is taken and `required` is false, a free port is used instead.
     */
    virtual Result<int> listen(const std::string& host, int port, bool required) = 0;

    /**
     * Waits for the browser to arrive at one of `paths` with the query parameter `state`, answers it with a page for the
     * user, and returns the query parameters as `{code?, state, iss?, error?, error_description?}`. Requests for other paths
     * get 404; requests with another `state` get an error page and are ignored. Fails with the code "timeout" or
     * "aborted".
     */
    virtual Result<Json> waitForCallback(const std::vector<std::string>& paths, const std::string& state, std::chrono::milliseconds timeout, const std::shared_ptr<AbortSignal>& signal) = 0;

    /** Stops listening. Idempotent. */
    virtual void close() = 0;
};
