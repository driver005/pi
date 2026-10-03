module;

#include <cstdint>

export module pi.support.frame_decoder;

import std;
export import pi.types.result;

/**
 * Incrementally splits arbitrary byte chunks into length-prefixed payloads (4-byte big-endian
 * length). An oversized declared length fails as soon as its header is complete; after a failure
 * or end() the decoder refuses further input. Port of packages/protocol/src/framing.ts.
 */
export class FrameDecoder {
public:
    static constexpr std::uint64_t kDefaultMaxFrameLength = 16 * 1024 * 1024;

    explicit FrameDecoder(std::uint64_t maxFrameLength = kDefaultMaxFrameLength)
        : m_maxFrameLength(maxFrameLength) {}

    /** Appends bytes and returns every frame they complete. */
    Result<std::vector<std::string>> push(std::string_view chunk) {
        if (m_ended) {
            return std::unexpected(Error{"frame", "Frame decoder has ended"});
        }
        if (m_failed) {
            return std::unexpected(Error{"frame", "Frame decoder has failed"});
        }
        std::vector<std::string> frames;
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            if (!m_expected) {
                const std::size_t take = std::min<std::size_t>(4 - m_header.size(), chunk.size() - offset);
                m_header.append(chunk.substr(offset, take));
                offset += take;
                if (m_header.size() < 4) {
                    continue;
                }
                std::uint64_t length = 0;
                for (const char c : m_header) {
                    length = (length << 8) | static_cast<unsigned char>(c);
                }
                m_header.clear();
                if (length > m_maxFrameLength) {
                    return std::unexpected(fail("Frame length " + std::to_string(length) + " exceeds configured limit of " +
                                                std::to_string(m_maxFrameLength))
                                               .error());
                }
                if (length == 0) {
                    frames.emplace_back();
                    continue;
                }
                m_expected = length;
                m_payload.clear();
            }
            const std::size_t take = std::min<std::size_t>(*m_expected - m_payload.size(), chunk.size() - offset);
            m_payload.append(chunk.substr(offset, take));
            offset += take;
            if (m_payload.size() == *m_expected) {
                frames.push_back(std::move(m_payload));
                m_payload.clear();
                m_expected.reset();
            }
        }
        return frames;
    }

    /** Declares the end of the stream; a partial frame is an error. */
    Result<void> end() {
        if (m_ended) {
            return std::unexpected(Error{"frame", "Frame decoder has ended"});
        }
        if (m_failed) {
            return std::unexpected(Error{"frame", "Frame decoder has failed"});
        }
        if (!m_header.empty() || m_expected) {
            return fail("Truncated frame at end of stream");
        }
        m_ended = true;
        return {};
    }

private:
    Result<void> fail(const std::string& message) {
        m_failed = true;
        m_header.clear();
        m_payload.clear();
        m_expected.reset();
        return std::unexpected(Error{"frame", message});
    }

    std::uint64_t m_maxFrameLength;
    std::string m_header;
    std::optional<std::uint64_t> m_expected;
    std::string m_payload;
    bool m_ended = false;
    bool m_failed = false;
};
