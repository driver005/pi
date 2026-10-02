export module pi.support.jsonl_line_reader;

import std;

/**
 * Splits a byte stream into JSONL records. Framing is LF only: a payload string may contain other
 * Unicode line separators (U+2028, U+2029), so readers that split on them are wrong. A trailing CR
 * is dropped, as is an empty record.
 */
export class JsonlLineReader {
public:
    /** Complete lines contained in the data so far. */
    std::vector<std::string> feed(std::string_view chunk);

    /** The unterminated last line at end of stream, if any. */
    std::optional<std::string> finish();

private:
    std::string trimCarriageReturn(std::string line) const;

    std::string m_buffer;
};

std::string JsonlLineReader::trimCarriageReturn(std::string line) const {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::vector<std::string> JsonlLineReader::feed(std::string_view chunk) {
    m_buffer.append(chunk);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t at = m_buffer.find('\n', start); at != std::string::npos; at = m_buffer.find('\n', start)) {
        std::string line = trimCarriageReturn(m_buffer.substr(start, at - start));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        start = at + 1;
    }
    m_buffer.erase(0, start);
    return lines;
}

std::optional<std::string> JsonlLineReader::finish() {
    std::string line = trimCarriageReturn(std::exchange(m_buffer, std::string()));
    if (line.empty()) {
        return std::nullopt;
    }
    return line;
}
