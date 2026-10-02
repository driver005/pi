module;

#include <cstdint>

export module pi.support.frame_encoder;

import std;
export import pi.types.result;

/** Prefixes a payload with its unsigned 32-bit big-endian byte length. */
export class FrameEncoder {
public:
    Result<std::string> encode(std::string_view payload) const;
};

Result<std::string> FrameEncoder::encode(std::string_view payload) const {
    if (payload.size() > 0xFFFFFFFFULL) {
        return std::unexpected(Error{"frame", "Frame payload exceeds the unsigned 32-bit length limit"});
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.reserve(4 + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((length >> shift) & 0xFF));
    }
    frame.append(payload);
    return frame;
}
