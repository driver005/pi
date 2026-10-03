export module pi.support.output_sanitizer;

import std;

/**
 * Cleans a stream of command output chunk by chunk: removes ANSI escape sequences, control and
 * binary characters and carriage returns, and holds back an incomplete UTF-8 sequence at the end
 * of a chunk until its remaining bytes arrive. One instance per command.
 */
export class OutputSanitizer {
public:
    OutputSanitizer()
        : m_csi("(?:\x1B|\xC2\x9B)[\\[\\]()#;?]*(?:\\d{1,4}(?:[;:]\\d{0,4})*)?[\\dA-PR-TZcf-nq-uy=><~]") {}

    std::string feed(std::string_view chunk) {
        const std::string complete = takeCompleteUtf8(chunk);
        return stripControls(std::regex_replace(stripOsc(complete), m_csi, ""));
    }

private:
    std::string takeCompleteUtf8(std::string_view chunk) {
        std::string data = m_pending;
        data.append(chunk);
        m_pending.clear();
        std::size_t back = 0;
        for (std::size_t i = data.size(); i > 0 && back < 4; --i, ++back) {
            const auto byte = static_cast<unsigned char>(data[i - 1]);
            if ((byte & 0xC0) == 0x80) {
                continue;
            }
            const std::size_t need = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
            if (need > back + 1) {
                m_pending = data.substr(i - 1);
                data.resize(i - 1);
            }
            break;
        }
        return data;
    }

    std::string stripOsc(const std::string& text) const {
        std::string out;
        std::size_t i = 0;
        while (i < text.size()) {
            if (text.compare(i, 2, "\x1B]") != 0) {
                out += text[i++];
                continue;
            }
            std::size_t end = std::string::npos;
            std::size_t length = 0;
            for (std::size_t j = i + 2; j < text.size() && end == std::string::npos; ++j) {
                if (text[j] == '\x07') {
                    end = j;
                    length = 1;
                } else if (text.compare(j, 2, "\x1B\\") == 0 || text.compare(j, 2, "\xC2\x9C") == 0) {
                    end = j;
                    length = 2;
                }
            }
            if (end == std::string::npos) {
                out += text[i++];
                continue;
            }
            i = end + length;
        }
        return out;
    }

    std::string stripControls(const std::string& text) const {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto byte = static_cast<unsigned char>(text[i]);
            const bool control = byte <= 0x08 || byte == 0x0B || byte == 0x0C || (byte >= 0x0E && byte <= 0x1F) ||
                                 byte == '\r';
            const bool specials = text.compare(i, 3, "\xEF\xBF\xB9") == 0 || text.compare(i, 3, "\xEF\xBF\xBA") == 0 ||
                                  text.compare(i, 3, "\xEF\xBF\xBB") == 0;
            if (specials) {
                i += 2;
            } else if (!control) {
                out += text[i];
            }
        }
        return out;
    }

    std::string m_pending;
    std::regex m_csi;
};

/** OSC sequences run from ESC ] to BEL, ESC \ or U+009C. Unterminated ones stay. */
