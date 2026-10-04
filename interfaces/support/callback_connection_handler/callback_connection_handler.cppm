export module pi.support.callback_connection_handler;

import std;
export import pi.server.i_byte_connection_handler;
export import pi.types.client_transport_handlers;

/** Adapts ClientTransportHandlers (callbacks) to the IByteConnectionHandler a socket connection feeds. */
export class CallbackConnectionHandler : public IByteConnectionHandler {
public:
    explicit CallbackConnectionHandler(ClientTransportHandlers handlers)
        : m_handlers(std::move(handlers)) {}

    void onData(std::string_view chunk) override {
        if (m_handlers.onData) {
            m_handlers.onData(chunk);
        }
    }

    void onClose() override {
        if (m_handlers.onClose) {
            m_handlers.onClose();
        }
    }

    void onError(const Error& error) override {
        if (m_handlers.onError) {
            m_handlers.onError(error);
        }
    }

private:
    ClientTransportHandlers m_handlers;
};
