module;

#include <cstdint>

export module pi.support.aws_event_stream_parser;

import std;
export import pi.types.aws_event_stream_message;
export import pi.types.result;

/**
 * Incremental decoder of the application/vnd.amazon.eventstream binary framing: a 12-byte prelude
 * (total length, headers length, prelude CRC), typed headers, payload and a trailing message CRC.
 * Both checksums are verified. Only string headers are kept.
 */
export class AwsEventStreamParser {
public:
    AwsEventStreamParser();

    /** Appends bytes and returns every message completed by them; an error leaves the parser unusable. */
    Result<std::vector<AwsEventStreamMessage>> feed(std::string_view chunk);

    /** An error when bytes of an incomplete message are left over. */
    Result<void> finish() const;

    std::uint32_t crc32(std::string_view data) const;

private:
    Result<AwsEventStreamMessage> decode(const std::string& frame) const;
    Result<void> readHeaders(const std::string& frame, std::size_t begin, std::size_t end,
                             AwsEventStreamMessage& message) const;
    std::uint32_t readUint32(const std::string& data, std::size_t offset) const;
    std::uint16_t readUint16(const std::string& data, std::size_t offset) const;

    std::array<std::uint32_t, 256> m_table{};
    std::string m_buffer;
    bool m_failed = false;
};

AwsEventStreamParser::AwsEventStreamParser() {
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
        }
        m_table[i] = value;
    }
}

std::uint32_t AwsEventStreamParser::crc32(std::string_view data) const {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char c : data) {
        crc = m_table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t AwsEventStreamParser::readUint32(const std::string& data, std::size_t offset) const {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
    }
    return value;
}

std::uint16_t AwsEventStreamParser::readUint16(const std::string& data, std::size_t offset) const {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(data[offset]) << 8) |
                                      static_cast<unsigned char>(data[offset + 1]));
}

Result<void> AwsEventStreamParser::readHeaders(const std::string& frame, std::size_t begin, std::size_t end,
                                               AwsEventStreamMessage& message) const {
    // Fixed value sizes of the non-string header types; 6 (bytes) and 7 (string) carry a length.
    const std::map<int, std::size_t> fixed{{0, 0}, {1, 0}, {2, 1}, {3, 2}, {4, 4}, {5, 8}, {8, 8}, {9, 16}};
    std::size_t pos = begin;
    while (pos < end) {
        const std::size_t nameLength = static_cast<unsigned char>(frame[pos++]);
        if (pos + nameLength + 1 > end) {
            return std::unexpected(Error{"eventstream", "Truncated event stream header"});
        }
        const std::string name = frame.substr(pos, nameLength);
        pos += nameLength;
        const int type = static_cast<unsigned char>(frame[pos++]);
        if (type == 6 || type == 7) {
            if (pos + 2 > end) {
                return std::unexpected(Error{"eventstream", "Truncated event stream header"});
            }
            const std::size_t length = readUint16(frame, pos);
            pos += 2;
            if (pos + length > end) {
                return std::unexpected(Error{"eventstream", "Truncated event stream header"});
            }
            if (type == 7) {
                message.headers[name] = frame.substr(pos, length);
            }
            pos += length;
            continue;
        }
        const auto size = fixed.find(type);
        if (size == fixed.end() || pos + size->second > end) {
            return std::unexpected(Error{"eventstream", "Unknown event stream header type " + std::to_string(type)});
        }
        pos += size->second;
    }
    return {};
}

Result<AwsEventStreamMessage> AwsEventStreamParser::decode(const std::string& frame) const {
    const std::uint32_t total = readUint32(frame, 0);
    const std::uint32_t headersLength = readUint32(frame, 4);
    if (crc32(std::string_view(frame).substr(0, 8)) != readUint32(frame, 8)) {
        return std::unexpected(Error{"eventstream", "Event stream prelude checksum mismatch"});
    }
    if (crc32(std::string_view(frame).substr(0, total - 4)) != readUint32(frame, total - 4)) {
        return std::unexpected(Error{"eventstream", "Event stream message checksum mismatch"});
    }
    if (12ULL + headersLength + 4ULL > total) {
        return std::unexpected(Error{"eventstream", "Invalid event stream header length"});
    }
    AwsEventStreamMessage message;
    if (auto headers = readHeaders(frame, 12, 12 + headersLength, message); !headers) {
        return std::unexpected(headers.error());
    }
    message.payload = frame.substr(12 + headersLength, total - 16 - headersLength);
    return message;
}

Result<std::vector<AwsEventStreamMessage>> AwsEventStreamParser::feed(std::string_view chunk) {
    if (m_failed) {
        return std::unexpected(Error{"eventstream", "Event stream is corrupt"});
    }
    m_buffer.append(chunk);
    std::vector<AwsEventStreamMessage> out;
    while (m_buffer.size() >= 12) {
        const std::uint32_t total = readUint32(m_buffer, 0);
        if (total < 16 || total > 24U * 1024U * 1024U) {
            m_failed = true;
            return std::unexpected(Error{"eventstream", "Invalid event stream message length"});
        }
        if (m_buffer.size() < total) {
            break;
        }
        auto message = decode(m_buffer.substr(0, total));
        if (!message) {
            m_failed = true;
            return std::unexpected(message.error());
        }
        out.push_back(std::move(*message));
        m_buffer.erase(0, total);
    }
    return out;
}

Result<void> AwsEventStreamParser::finish() const {
    if (!m_buffer.empty()) {
        return std::unexpected(Error{"eventstream", "Event stream ended inside a message"});
    }
    return {};
}
