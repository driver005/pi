export module pi.server.i_byte_connection;

import std;
export import pi.types.result;

/**
 * An established, authorized, ordered byte connection. send() queues the bytes and returns without
 * waiting for the peer (it fails when the connection is closed or the peer is too slow); close()
 * delivers an optional final chunk, then ends the connection. Thread-safe.
 */
export class IByteConnection {
public:
    virtual ~IByteConnection() = default;

    virtual bool closed() const = 0;
    virtual Result<void> send(std::string_view chunk) = 0;
    virtual void close(std::string_view finalChunk = {}) = 0;
};
