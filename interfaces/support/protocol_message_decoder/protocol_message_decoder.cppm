module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.protocol_message_decoder;

import std;
export import pi.support.frame_decoder;
export import pi.support.protocol_codec;

/**
 * Incrementally decodes and validates framed messages of one side: push byte chunks, receive whole
 * validated messages. The first error is final; later calls report that the decoder has failed.
 * Port of ClientMessageDecoder / ServerMessageDecoder in packages/protocol/src/codec.ts.
 */
export class ProtocolMessageDecoder {
public:
    explicit ProtocolMessageDecoder(ProtocolSide side,
                                    std::uint64_t maxFrameLength = FrameDecoder::kDefaultMaxFrameLength);

    Result<std::vector<Json>> push(std::string_view chunk);
    Result<void> end();

private:
    Error failed(const std::string& message);

    ProtocolSide m_side;
    std::uint64_t m_maxFrameLength;
    FrameDecoder m_frames;
    ProtocolCodec m_codec;
    bool m_failed = false;
};

ProtocolMessageDecoder::ProtocolMessageDecoder(ProtocolSide side, std::uint64_t maxFrameLength)
    : m_side(side), m_maxFrameLength(maxFrameLength), m_frames(maxFrameLength) {}

Error ProtocolMessageDecoder::failed(const std::string& message) {
    m_failed = true;
    return Error{"protocol_validation", message};
}

Result<std::vector<Json>> ProtocolMessageDecoder::push(std::string_view chunk) {
    const std::string side = m_side == ProtocolSide::Client ? "client" : "server";
    if (m_failed) {
        return std::unexpected(Error{"protocol_validation", side + " message decoder has failed"});
    }
    auto payloads = m_frames.push(chunk);
    if (!payloads) {
        return std::unexpected(failed("Invalid " + side + " protocol frame: " + payloads.error().message));
    }
    std::vector<Json> messages;
    for (const std::string& payload : *payloads) {
        auto message = m_codec.decode(m_side, payload, m_maxFrameLength);
        if (!message) {
            return std::unexpected(failed(message.error().message));
        }
        messages.push_back(std::move(*message));
    }
    return messages;
}

Result<void> ProtocolMessageDecoder::end() {
    const std::string side = m_side == ProtocolSide::Client ? "client" : "server";
    if (m_failed) {
        return std::unexpected(Error{"protocol_validation", side + " message decoder has failed"});
    }
    if (auto ended = m_frames.end(); !ended) {
        return std::unexpected(failed("Invalid " + side + " protocol framing: " + ended.error().message));
    }
    return {};
}
