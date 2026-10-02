export module pi.support.output_accumulator;

import std;
import pi.platform.i_crypto;
import pi.platform.i_file_system;
import pi.support.text_truncator;
export import pi.types.full_output;
export import pi.types.output_snapshot;

/**
 * Tracks streaming command output with bounded memory (port of core/tools/output-accumulator.ts):
 * keeps a rolling tail for display snapshots, counts lines/bytes of everything, and spills the
 * complete output to a temp file once it exceeds the limits.
 */
export class OutputAccumulator {
public:
    OutputAccumulator(IFileSystem& fileSystem, ICrypto& crypto, std::string tempFilePrefix,
                      std::int64_t maxLines = TextTruncator::kDefaultMaxLines,
                      std::int64_t maxBytes = TextTruncator::kDefaultMaxBytes)
        : m_fileSystem(fileSystem),
          m_crypto(crypto),
          m_prefix(std::move(tempFilePrefix)),
          m_maxLines(maxLines),
          m_maxBytes(maxBytes) {}

    void append(std::string_view data) {
        if (data.empty()) {
            return;
        }
        m_totalBytes += static_cast<std::int64_t>(data.size());
        appendTail(data);
        countLines(data);
        if (!m_tempPath.empty() || exceedsLimits()) {
            ensureTempFile();
            m_fileSystem.appendFile(m_tempPath, std::string(data));
        } else {
            m_inMemory.append(data);
        }
    }

    void finish() {
        if (exceedsLimits()) {
            ensureTempFile();
        }
    }

    OutputSnapshot snapshot(bool persistIfTruncated) {
        const TruncationResult tail = m_truncator.truncateTail(m_tail, m_maxLines, m_maxBytes);
        const bool truncated = totalLines() > m_maxLines || m_totalBytes > m_maxBytes;
        TruncationResult truncation = tail;
        truncation.truncated = truncated;
        truncation.truncatedBy = truncated ? (!tail.truncatedBy.empty() ? tail.truncatedBy
                                                                        : (m_totalBytes > m_maxBytes ? "bytes" : "lines"))
                                           : "";
        truncation.totalLines = totalLines();
        truncation.totalBytes = m_totalBytes;
        truncation.maxLines = m_maxLines;
        truncation.maxBytes = m_maxBytes;
        if (persistIfTruncated && truncated) {
            ensureTempFile();
        }
        OutputSnapshot result;
        result.content = truncation.content;
        result.truncation = truncation;
        if (!m_tempPath.empty()) {
            result.fullOutputPath = m_tempPath;
        }
        return result;
    }

    /** Bytes in the current (last) line, for "line is N KB" notices. */
    std::int64_t lastLineBytes() const {
        return m_currentLineBytes;
    }

    /**
     * Complete output; longer than `maxBytes` keeps the first and last maxBytes/2 around an
     * omission marker.
     */
    FullOutput readFullOutput(std::int64_t maxBytes) {
        if (m_tempPath.empty()) {
            return FullOutput{m_inMemory, false};
        }
        auto content = m_fileSystem.readFile(m_tempPath);
        if (!content.has_value()) {
            return FullOutput{m_tail, true};
        }
        if (static_cast<std::int64_t>(content->size()) <= maxBytes) {
            return FullOutput{*content, false};
        }
        const std::size_t headBytes = static_cast<std::size_t>(maxBytes / 2);
        const std::size_t tailBytes = static_cast<std::size_t>(maxBytes) - headBytes;
        std::size_t headEnd = headBytes;
        while (headEnd > 0 && (static_cast<unsigned char>((*content)[headEnd]) & 0xC0) == 0x80) {
            --headEnd;
        }
        std::size_t tailStart = content->size() - tailBytes;
        while (tailStart < content->size() && (static_cast<unsigned char>((*content)[tailStart]) & 0xC0) == 0x80) {
            ++tailStart;
        }
        const std::size_t omitted = tailStart - headEnd;
        return FullOutput{content->substr(0, headEnd) + "\n\n[... " + std::to_string(omitted) +
                              " bytes omitted ...]\n\n" + content->substr(tailStart),
                          true};
    }

    const std::string& tempFilePath() const {
        return m_tempPath;
    }

private:
    bool exceedsLimits() const {
        return m_totalBytes > m_maxBytes || totalLines() > m_maxLines;
    }

    std::int64_t totalLines() const {
        return m_newlines + (m_currentLineBytes > 0 ? 1 : 0);
    }

    void appendTail(std::string_view data) {
        m_tail.append(data);
        const auto rolling = static_cast<std::size_t>(std::max<std::int64_t>(m_maxBytes * 2, 1));
        if (m_tail.size() > rolling * 2) {
            std::size_t cut = m_tail.size() - rolling;
            while (cut < m_tail.size() && (static_cast<unsigned char>(m_tail[cut]) & 0xC0) == 0x80) {
                ++cut;
            }
            m_tail.erase(0, cut);
        }
    }

    void countLines(std::string_view data) {
        for (const char c : data) {
            if (c == '\n') {
                ++m_newlines;
                m_currentLineBytes = 0;
            } else {
                ++m_currentLineBytes;
            }
        }
    }

    void ensureTempFile() {
        if (!m_tempPath.empty()) {
            return;
        }
        const std::string random = m_crypto.randomBytes(8);
        std::string id;
        for (const unsigned char byte : random) {
            id += "0123456789abcdef"[byte >> 4];
            id += "0123456789abcdef"[byte & 0xF];
        }
        m_tempPath = (std::filesystem::temp_directory_path() / (m_prefix + "-" + id + ".log")).string();
        m_fileSystem.writeFile(m_tempPath, m_inMemory);
        m_inMemory.clear();
    }

    IFileSystem& m_fileSystem;
    ICrypto& m_crypto;
    std::string m_prefix;
    std::int64_t m_maxLines;
    std::int64_t m_maxBytes;
    TextTruncator m_truncator;
    std::string m_tail;
    std::string m_inMemory;
    std::string m_tempPath;
    std::int64_t m_totalBytes = 0;
    std::int64_t m_newlines = 0;
    std::int64_t m_currentLineBytes = 0;
};
