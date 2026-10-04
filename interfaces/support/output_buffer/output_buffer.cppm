module;

#include <cstdint>

export module pi.support.output_buffer;

import std;
export import pi.support.output_bounder;
export import pi.types.bounded_output;
export import pi.types.output_chunk;
export import pi.types.output_limits;

/**
 * Bounded running output of one tool call. Accepting a chunk costs time proportional to the chunk: head retention
 * stops storing once the window is full, and tail retention drops stored text the window no longer needs when it
 * snapshots. Counts of the whole stream are kept so the dropped totals stay exact. Port of OutputBuffer in
 * packages/durable/src/harness/output.ts.
 */
export class OutputBuffer {
public:
    explicit OutputBuffer(OutputLimits limits) : m_limits(std::move(limits)) {}

    /** Bytes currently held; bounded by the limits plus one chunk. */
    std::int64_t storedBytes() const {
        return m_storedBytes;
    }

    /** Accepts a chunk of UTF-8 bytes; an incomplete trailing character waits for the next chunk. Whether anything was accepted. */
    bool push(const std::string& chunk) {
        std::string data = m_pending + chunk;
        m_pending.clear();
        const std::size_t keep = incompleteTail(data);
        m_pending = data.substr(data.size() - keep);
        data.resize(data.size() - keep);
        return accept(data);
    }

    /** Flushes an incomplete trailing character as a replacement character; call when the stream ends. */
    void end() {
        if (!m_pending.empty()) {
            m_pending.clear();
            accept("\xEF\xBF\xBD");
        }
    }

    /** The retained, sanitized output and what the limits dropped from the whole stream. */
    BoundedOutput snapshot() {
        std::string stored;
        for (const OutputChunk& chunk : m_chunks) {
            stored += chunk.text;
        }
        const OutputSlice kept = m_bounder.bound(stored, m_limits);
        const std::int64_t storedLines = m_bounder.lines(m_storedNewlines, stored.empty() || stored.back() == '\n');
        const std::int64_t keptLines = storedLines - kept.droppedLines;
        // Tail windows never reach back before this one, so only the kept slice needs storing.
        if (m_limits.retain == "tail" || m_chunks.size() > 1) {
            const std::string text = m_limits.retain == "tail" ? kept.text : stored;
            const std::int64_t bytes = m_limits.retain == "tail" ? kept.bytes : m_storedBytes;
            m_chunks.clear();
            if (!text.empty()) {
                m_chunks.push_back(OutputChunk{text, bytes, m_bounder.countNewlines(text)});
            }
            m_storedBytes = bytes;
            m_storedNewlines = m_chunks.empty() ? 0 : m_chunks.front().newlines;
        }
        BoundedOutput output;
        output.text = m_bounder.sanitize(kept.text);
        output.droppedBytes = m_totalBytes - kept.bytes;
        output.droppedLines = m_bounder.lines(m_totalNewlines, m_endsWithNewline) - keptLines;
        return output;
    }

private:
    /** The number of trailing bytes that start a character not yet complete. */
    std::size_t incompleteTail(const std::string& data) const {
        std::size_t back = 0;
        for (std::size_t i = data.size(); i > 0 && back < 4; --i, ++back) {
            const auto byte = static_cast<unsigned char>(data[i - 1]);
            if ((byte & 0xC0) == 0x80) {
                continue;
            }
            const std::size_t need = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
            return need > back + 1 ? back + 1 : 0;
        }
        return 0;
    }

    bool accept(const std::string& text) {
        if (text.empty()) {
            return false;
        }
        const auto bytes = static_cast<std::int64_t>(text.size());
        const std::int64_t newlines = m_bounder.countNewlines(text);
        m_totalBytes += bytes;
        m_totalNewlines += newlines;
        m_endsWithNewline = text.back() == '\n';
        if (m_full) {
            return true;
        }
        m_chunks.push_back(OutputChunk{text, bytes, newlines});
        m_storedBytes += bytes;
        m_storedNewlines += newlines;
        if (m_limits.retain == "head") {
            // Nothing past a full window is ever needed.
            m_full = m_storedBytes > m_limits.maxBytes || m_storedNewlines >= m_limits.maxLines;
            return true;
        }
        // Drop leading chunks while the rest still holds more than a window: more than `maxBytes` bytes or
        // `maxLines` newlines, plus one, so the window's line start can still be found. Each chunk is dropped once.
        while (m_chunks.size() > 1) {
            const OutputChunk& first = m_chunks.front();
            const std::int64_t bytesAfter = m_storedBytes - first.bytes;
            const std::int64_t newlinesAfter = m_storedNewlines - first.newlines;
            if (bytesAfter <= m_limits.maxBytes + 1 && newlinesAfter <= m_limits.maxLines + 1) {
                break;
            }
            m_chunks.pop_front();
            m_storedBytes = bytesAfter;
            m_storedNewlines = newlinesAfter;
        }
        return true;
    }

    OutputLimits m_limits;
    OutputBounder m_bounder;
    std::string m_pending;
    /** Stored chunks: for head the start of the stream, for tail a suffix that still contains the next window. */
    std::deque<OutputChunk> m_chunks;
    std::int64_t m_storedBytes = 0;
    std::int64_t m_storedNewlines = 0;
    bool m_full = false;
    std::int64_t m_totalBytes = 0;
    std::int64_t m_totalNewlines = 0;
    bool m_endsWithNewline = true;
};
