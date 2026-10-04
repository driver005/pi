export module pi.server.i_server_listener;

import std;
export import pi.server.i_byte_connection;
export import pi.server.i_byte_connection_handler;
export import pi.types.result;

/** Supplies established byte connections after any required transport authentication. */
export class IServerListener {
public:
    using Acceptor = std::function<std::shared_ptr<IByteConnectionHandler>(std::shared_ptr<IByteConnection>)>;

    virtual ~IServerListener() = default;

    /** Starts listening; every authorized connection is handed to `accept`. */
    virtual Result<void> start(Acceptor accept) = 0;
    /** Stops listening and closes the connections it accepted. Idempotent. */
    virtual void close() = 0;
};
