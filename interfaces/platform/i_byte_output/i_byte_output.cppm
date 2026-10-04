export module pi.platform.i_byte_output;

import std;

/** A writable byte stream (stdout, a socket). Writes from different threads must not interleave. */
export class IByteOutput {
public:
    virtual ~IByteOutput() = default;

    virtual void write(std::string_view bytes) = 0;
    virtual void flush() = 0;
};
