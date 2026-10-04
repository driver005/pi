export module pi.server.i_byte_connection_handler;

import std;
export import pi.types.error;

/** Receives what happens on one connection; called from the connection's reader thread, in order. */
export class IByteConnectionHandler {
public:
    virtual ~IByteConnectionHandler() = default;

    virtual void onData(std::string_view chunk) = 0;
    virtual void onClose() = 0;
    virtual void onError(const Error& error) = 0;
};
