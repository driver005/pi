export module pi.types.mcp_http_options;

import std;
export import pi.types.http_headers;

/** How to reach a streamable HTTP MCP server. */
export struct McpHttpOptions {
    std::string url;
    /** Sent with every request (static auth headers and the like). */
    HttpHeaders headers;
    /** Returns a bearer token, or an empty string for none; called before every request. */
    std::function<std::string()> bearerToken;
    /**
     * Called with the `WWW-Authenticate` header and the bearer token of a request the server answered with 401. True means
     * the token was replaced (refreshed): the request is sent once more with the new `bearerToken()`.
     */
    std::function<bool(const std::string& wwwAuthenticate, const std::string& staleToken)> onUnauthorized;
    /** Open the server-to-client GET stream once the session is initialized. */
    bool openGetStream = true;
    std::size_t maxMessageBytes = 16 * 1024 * 1024;
    std::int64_t reconnectInitialDelayMs = 1000;
    std::int64_t reconnectMaxDelayMs = 30000;
    int reconnectMaxRetries = 5;
    /** How long the DELETE that ends the session may take on close. */
    std::int64_t closeTimeoutMs = 1000;
};
