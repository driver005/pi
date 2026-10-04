module;

#include <cstdint>

export module pi.support.output_bounder;

import std;
export import pi.types.output_limits;
export import pi.types.output_slice;

/**
 * Bounds tool output to whole lines within byte and line limits. Port of boundOutput()/sanitizeOutput() in
 * packages/durable/src/harness/output.ts; text is UTF-8 and a cut never splits a character.
 */
export class OutputBounder {
public:
    /** Removes control characters that break display and transcripts; tabs and newlines stay. */
    std::string sanitize(const std::string& text) const {
        std::string clean;
        clean.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte <= 0x08 || (byte >= 0x0b && byte <= 0x1f)) {
                continue;
            }
            // U+FFF9..U+FFFB, the interlinear annotation characters: EF BF B9..BB.
            if (byte == 0xEF && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xBF) {
                const auto last = static_cast<unsigned char>(text[i + 2]);
                if (last >= 0xB9 && last <= 0xBB) {
                    i += 2;
                    continue;
                }
            }
            clean.push_back(text[i]);
        }
        return clean;
    }

    /**
     * Bounds `text` to whole lines within the limits: the first lines for `head`, the last lines for `tail`. The
     * result is an exact slice, trailing newline included. A single line longer than `maxBytes` is cut at the byte
     * limit on a character boundary.
     */
    OutputSlice bound(const std::string& text, const OutputLimits& limits) const {
        const auto [from, to] = limits.retain == "head" ? headRange(text, limits) : tailRange(text, limits);
        const std::string kept = text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
        OutputSlice slice;
        slice.text = kept.size() == text.size() ? text : kept;
        slice.bytes = static_cast<std::int64_t>(kept.size());
        slice.droppedBytes = static_cast<std::int64_t>(text.size() - kept.size());
        slice.droppedLines = lineCount(text) - lineCount(kept);
        return slice;
    }

    /** Lines of text with `newlines` newlines; a final unterminated line counts. */
    std::int64_t lines(std::int64_t newlines, bool terminated) const {
        return newlines + (terminated ? 0 : 1);
    }

    /** The number of newline bytes in `text`. */
    std::int64_t countNewlines(const std::string& text) const {
        return static_cast<std::int64_t>(std::count(text.begin(), text.end(), '\n'));
    }

private:
    std::pair<std::int64_t, std::int64_t> headRange(const std::string& bytes, const OutputLimits& limits) const {
        if (limits.maxLines == 0 || limits.maxBytes == 0) {
            return {0, 0};
        }
        std::int64_t end = static_cast<std::int64_t>(bytes.size());
        std::int64_t lines = 0;
        for (std::size_t index = bytes.find('\n'); index != std::string::npos; index = bytes.find('\n', index + 1)) {
            if (++lines == limits.maxLines) {
                end = static_cast<std::int64_t>(index) + 1;
                break;
            }
        }
        if (end > limits.maxBytes) {
            const std::size_t newline = bytes.rfind('\n', static_cast<std::size_t>(limits.maxBytes - 1));
            end = newline == std::string::npos ? characterEnd(bytes, limits.maxBytes) : static_cast<std::int64_t>(newline) + 1;
        }
        return {0, end};
    }

    std::pair<std::int64_t, std::int64_t> tailRange(const std::string& bytes, const OutputLimits& limits) const {
        const auto size = static_cast<std::int64_t>(bytes.size());
        if (limits.maxLines == 0 || limits.maxBytes == 0) {
            return {size, size};
        }
        // A trailing newline ends the last line rather than starting another.
        const std::int64_t last = (size > 0 && bytes[static_cast<std::size_t>(size - 1)] == '\n') ? size - 2 : size - 1;
        std::int64_t start = 0;
        std::int64_t lines = 1;
        std::int64_t index = last < 0 ? -1 : lastNewline(bytes, last);
        while (index != -1) {
            if (lines == limits.maxLines) {
                start = index + 1;
                break;
            }
            ++lines;
            index = index == 0 ? -1 : lastNewline(bytes, index - 1);
        }
        if (size - start > limits.maxBytes) {
            const std::int64_t from = size - limits.maxBytes;
            const std::size_t newline = bytes.find('\n', static_cast<std::size_t>(from - 1));
            start = newline != std::string::npos && static_cast<std::int64_t>(newline) + 1 < size
                        ? static_cast<std::int64_t>(newline) + 1
                        : characterStart(bytes, from);
        }
        return {start, size};
    }

    /** The last newline at or before `from`, or -1. */
    std::int64_t lastNewline(const std::string& bytes, std::int64_t from) const {
        const std::size_t found = bytes.rfind('\n', static_cast<std::size_t>(from));
        return found == std::string::npos ? -1 : static_cast<std::int64_t>(found);
    }

    /** The last character boundary at or before `index`. */
    std::int64_t characterEnd(const std::string& bytes, std::int64_t index) const {
        std::int64_t end = index;
        while (end > 0 && end < static_cast<std::int64_t>(bytes.size()) &&
               (static_cast<unsigned char>(bytes[static_cast<std::size_t>(end)]) & 0xC0) == 0x80) {
            --end;
        }
        return end;
    }

    /** The first character boundary at or after `index`. */
    std::int64_t characterStart(const std::string& bytes, std::int64_t index) const {
        std::int64_t start = index;
        while (start < static_cast<std::int64_t>(bytes.size()) &&
               (static_cast<unsigned char>(bytes[static_cast<std::size_t>(start)]) & 0xC0) == 0x80) {
            ++start;
        }
        return start;
    }

    std::int64_t lineCount(const std::string& bytes) const {
        if (bytes.empty()) {
            return 0;
        }
        return countNewlines(bytes) + (bytes.back() == '\n' ? 0 : 1);
    }
};
