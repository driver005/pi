export module pi.platform.i_byte_input;

import std;

/** A readable byte stream (stdin, a socket). */
export class IByteInput {
public:
    virtual ~IByteInput() = default;

    /** Blocks for the next chunk; nullopt at end of stream. */
    virtual std::optional<std::string> read() = 0;
};
