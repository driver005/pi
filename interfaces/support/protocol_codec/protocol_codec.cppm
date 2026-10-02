module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.protocol_codec;

import std;
export import pi.support.cbor_decoder;
export import pi.support.cbor_encoder;
export import pi.support.frame_decoder;
export import pi.support.frame_encoder;
export import pi.support.protocol_validator;

/**
 * Validates, CBOR-encodes and frames one protocol message, and decodes one CBOR payload back into a
 * validated message. Errors use the code "protocol_validation". Port of the encode/parse functions of
 * packages/protocol/src/codec.ts.
 */
export class ProtocolCodec {
public:
    /** One complete length-prefixed frame. */
    Result<std::string> encode(ProtocolSide side, const Json& message,
                               std::uint64_t maxFrameLength = FrameDecoder::kDefaultMaxFrameLength) const;
    /** Decodes and validates the CBOR payload of one frame. */
    Result<Json> decode(ProtocolSide side, std::string_view payload,
                        std::uint64_t maxFrameLength = FrameDecoder::kDefaultMaxFrameLength) const;

private:
    std::string sideName(ProtocolSide side) const;
    std::string bounded(const std::string& message) const;

    ProtocolValidator m_validator;
    CborEncoder m_encoder;
    CborDecoder m_decoder;
    FrameEncoder m_frames;
};

std::string ProtocolCodec::sideName(ProtocolSide side) const {
    return side == ProtocolSide::Client ? "client" : "server";
}

std::string ProtocolCodec::bounded(const std::string& message) const {
    return message.size() <= 500 ? message : message.substr(0, 497) + "...";
}

Result<std::string> ProtocolCodec::encode(ProtocolSide side, const Json& message, std::uint64_t maxFrameLength) const {
    if (auto valid = m_validator.validate(side, message); !valid) {
        return std::unexpected(valid.error());
    }
    CborOptions options;
    options.maxByteLength = maxFrameLength;
    auto payload = m_encoder.encode(message, options);
    if (!payload) {
        return std::unexpected(Error{"protocol_validation", "Unable to encode " + sideName(side) +
                                                                " protocol message: " + bounded(payload.error().message)});
    }
    auto frame = m_frames.encode(*payload);
    if (!frame) {
        return std::unexpected(Error{"protocol_validation", "Unable to encode " + sideName(side) +
                                                                " protocol message: " + bounded(frame.error().message)});
    }
    return frame;
}

Result<Json> ProtocolCodec::decode(ProtocolSide side, std::string_view payload, std::uint64_t maxFrameLength) const {
    CborOptions options;
    options.maxByteLength = maxFrameLength;
    auto message = m_decoder.decode(payload, options);
    if (!message) {
        return std::unexpected(Error{"protocol_validation", "Invalid " + sideName(side) +
                                                                " protocol frame: " + bounded(message.error().message)});
    }
    if (auto valid = m_validator.validate(side, *message); !valid) {
        return std::unexpected(valid.error());
    }
    return message;
}
