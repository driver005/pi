export module pi.client.i_client_transport;

import std;
export import pi.server.i_byte_connection;
export import pi.types.client_transport_handlers;
export import pi.types.result;

/** Opens connections to one server endpoint (a unix socket, a pipe, an in-memory peer). */
export class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    /**
     * Connects and starts delivering the peer's bytes to `handlers` on a thread of the transport. Returns the connection to
     * write to; after its close() (or the peer's) the handlers' onClose runs exactly once, and nothing is delivered later.
     */
    virtual Result<std::shared_ptr<IByteConnection>> connect(ClientTransportHandlers handlers) = 0;
};
