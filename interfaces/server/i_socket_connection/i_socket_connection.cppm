export module pi.server.i_socket_connection;

import std;
export import pi.server.i_byte_connection;
export import pi.server.i_byte_connection_handler;

/** An accepted socket: the IByteConnection a server writes to plus the read loop that feeds its handler. */
export class ISocketConnection : public IByteConnection {
public:
    /**
     * Blocks on the calling thread, delivering received bytes to `handler` in order, until the peer
     * or close() ends the connection; then reports onClose (or onError first, on a read failure).
     */
    virtual void run(std::shared_ptr<IByteConnectionHandler> handler) = 0;
};
