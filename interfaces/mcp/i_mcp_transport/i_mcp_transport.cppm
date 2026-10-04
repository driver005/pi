export module pi.mcp.i_mcp_transport;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * A bidirectional JSON-RPC message pipe to one MCP server (child process stdio, streamable HTTP,
 * in-memory in tests). Listeners are called on the transport's own threads and must not block on
 * the transport. close() is idempotent and ends with exactly one close notification.
 */
export class IMcpTransport {
public:
    using MessageListener = std::function<void(const Json&)>;
    using ErrorListener = std::function<void(const Error&)>;
    using CloseListener = std::function<void()>;

    virtual ~IMcpTransport() = default;

    virtual Result<void> start() = 0;
    virtual Result<void> send(const Json& message) = 0;
    virtual void close() = 0;

    virtual void setMessageListener(MessageListener listener) = 0;
    virtual void setErrorListener(ErrorListener listener) = 0;
    virtual void setCloseListener(CloseListener listener) = 0;

    /** Called after initialize with the negotiated protocol version (HTTP sends it as a header). */
    virtual void setProtocolVersion(const std::string& version) = 0;
};
