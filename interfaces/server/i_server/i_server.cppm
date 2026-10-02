export module pi.server.i_server;

import std;
export import pi.types.result;

/** A running protocol server: accepts connections from its listeners and serves them. */
export class IServer {
public:
    virtual ~IServer() = default;

    virtual const std::string& serverId() const = 0;
    virtual Result<void> start() = 0;
    /** Stops listening, disconnects every client and closes every routed session. Idempotent. */
    virtual Result<void> close() = 0;
};
