export module pi.types.client_transport_handlers;

import std;
export import pi.types.error;

/** What a client transport reports about its connection; called from the transport's reader thread, in order. */
export struct ClientTransportHandlers {
    std::function<void(std::string_view chunk)> onData;
    /** The last call of a connection: the peer or close() ended it. */
    std::function<void()> onClose;
    /** A read failure; onClose follows. */
    std::function<void(const Error& error)> onError;
};
