module;

#include <cstdint>
#include <cstdio>

export module pi.support.text_truncator;

import std;
export import pi.types.truncation_result;

/**
 * Output truncation shared by the tools (port of core/tools/truncate.ts). Two independent
 * limits, whichever is hit first: lines (default 2000) and bytes (default 50KB). Head truncation
 * never returns partial lines; tail truncation may cut one overlong last line.
 */
export class TextTruncator {
public:
    static constexpr std::int64_t kDefaultMaxLines = 2000;
    static constexpr std::int64_t kDefaultMaxBytes = 50 * 1024;
    static constexpr std::int64_t kGrepMaxLineLength = 500;

    std::string formatSize(std::int64_t bytes) const;
    TruncationResult truncateHead(const std::string& content, std::int64_t maxLines = kDefaultMaxLines,
                                  std::int64_t maxBytes = kDefaultMaxBytes) const;
    TruncationResult truncateTail(const std::string& content, std::int64_t maxLines = kDefaultMaxLines,
                                  std::int64_t maxBytes = kDefaultMaxBytes) const;
    /** Cuts to maxChars characters plus "... [truncated]"; second is whether it was cut. */
    std::pair<std::string, bool> truncateLine(const std::string& line,
                                              std::int64_t maxChars = kGrepMaxLineLength) const;

private:
    std::vector<std::string> splitLinesForCounting(const std::string& content) const;
    std::string join(const std::vector<std::string>& lines) const;
    std::string tailBytes(const std::string& text, std::int64_t maxBytes) const;
    TruncationResult untruncated(const std::string& content, std::int64_t lines, std::int64_t bytes,
                                 std::int64_t maxLines, std::int64_t maxBytes) const;
};

std::vector<std::string> TextTruncator::splitLinesForCounting(const std::string& content) const {
    std::vector<std::string> lines;
    if (content.empty()) {
        return lines;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = content.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, end - start));
        start = end + 1;
    }
    if (content.back() == '\n') {
        lines.pop_back();
    }
    return lines;
}

std::string TextTruncator::join(const std::vector<std::string>& lines) const {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += (i > 0 ? "\n" : "") + lines[i];
    }
    return out;
}

std::string TextTruncator::formatSize(std::int64_t bytes) const {
    char buffer[48];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%lldB", static_cast<long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1fKB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fMB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

TruncationResult TextTruncator::untruncated(const std::string& content, std::int64_t lines,
                                            std::int64_t bytes, std::int64_t maxLines,
                                            std::int64_t maxBytes) const {
    TruncationResult result;
    result.content = content;
    result.totalLines = lines;
    result.totalBytes = bytes;
    result.outputLines = lines;
    result.outputBytes = bytes;
    result.maxLines = maxLines;
    result.maxBytes = maxBytes;
    return result;
}

TruncationResult TextTruncator::truncateHead(const std::string& content, std::int64_t maxLines,
                                             std::int64_t maxBytes) const {
    const auto totalBytes = static_cast<std::int64_t>(content.size());
    const std::vector<std::string> lines = splitLinesForCounting(content);
    const auto totalLines = static_cast<std::int64_t>(lines.size());
    if (totalLines <= maxLines && totalBytes <= maxBytes) {
        return untruncated(content, totalLines, totalBytes, maxLines, maxBytes);
    }
    TruncationResult result = untruncated("", totalLines, totalBytes, maxLines, maxBytes);
    result.truncated = true;
    result.outputLines = 0;
    result.outputBytes = 0;
    if (static_cast<std::int64_t>(lines[0].size()) > maxBytes) {
        result.truncatedBy = "bytes";
        result.firstLineExceedsLimit = true;
        return result;
    }
    std::vector<std::string> kept;
    std::int64_t keptBytes = 0;
    std::string reason = "lines";
    for (std::size_t i = 0; i < lines.size() && static_cast<std::int64_t>(i) < maxLines; ++i) {
        const auto lineBytes = static_cast<std::int64_t>(lines[i].size()) + (i > 0 ? 1 : 0);
        if (keptBytes + lineBytes > maxBytes) {
            reason = "bytes";
            break;
        }
        kept.push_back(lines[i]);
        keptBytes += lineBytes;
    }
    if (static_cast<std::int64_t>(kept.size()) >= maxLines && keptBytes <= maxBytes) {
        reason = "lines";
    }
    result.content = join(kept);
    result.truncatedBy = reason;
    result.outputLines = static_cast<std::int64_t>(kept.size());
    result.outputBytes = static_cast<std::int64_t>(result.content.size());
    return result;
}

std::string TextTruncator::tailBytes(const std::string& text, std::int64_t maxBytes) const {
    if (static_cast<std::int64_t>(text.size()) <= maxBytes) {
        return text;
    }
    std::size_t start = text.size() - static_cast<std::size_t>(maxBytes);
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        ++start;
    }
    return text.substr(start);
}

TruncationResult TextTruncator::truncateTail(const std::string& content, std::int64_t maxLines,
                                             std::int64_t maxBytes) const {
    const auto totalBytes = static_cast<std::int64_t>(content.size());
    const std::vector<std::string> lines = splitLinesForCounting(content);
    const auto totalLines = static_cast<std::int64_t>(lines.size());
    if (totalLines <= maxLines && totalBytes <= maxBytes) {
        return untruncated(content, totalLines, totalBytes, maxLines, maxBytes);
    }
    std::vector<std::string> kept;
    std::int64_t keptBytes = 0;
    std::string reason = "lines";
    bool partial = false;
    for (auto i = static_cast<std::int64_t>(lines.size()) - 1;
         i >= 0 && static_cast<std::int64_t>(kept.size()) < maxLines; --i) {
        const std::string& line = lines[static_cast<std::size_t>(i)];
        const auto lineBytes = static_cast<std::int64_t>(line.size()) + (!kept.empty() ? 1 : 0);
        if (keptBytes + lineBytes > maxBytes) {
            reason = "bytes";
            if (kept.empty()) {
                kept.insert(kept.begin(), tailBytes(line, maxBytes));
                keptBytes = static_cast<std::int64_t>(kept.front().size());
                partial = true;
            }
            break;
        }
        kept.insert(kept.begin(), line);
        keptBytes += lineBytes;
    }
    if (static_cast<std::int64_t>(kept.size()) >= maxLines && keptBytes <= maxBytes) {
        reason = "lines";
    }
    TruncationResult result = untruncated(join(kept), totalLines, totalBytes, maxLines, maxBytes);
    result.truncated = true;
    result.truncatedBy = reason;
    result.outputLines = static_cast<std::int64_t>(kept.size());
    result.outputBytes = static_cast<std::int64_t>(result.content.size());
    result.lastLinePartial = partial;
    return result;
}

std::pair<std::string, bool> TextTruncator::truncateLine(const std::string& line,
                                                         std::int64_t maxChars) const {
    std::int64_t chars = 0;
    std::size_t cut = line.size();
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) {
                cut = i;
                break;
            }
            ++chars;
        }
    }
    if (cut == line.size()) {
        return {line, false};
    }
    return {line.substr(0, cut) + "... [truncated]", true};
}
