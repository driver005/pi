export module pi.support.text_trimmer;

import std;

/** Whitespace and byte order mark handling for text read from files and child processes. */
export class TextTrimmer {
public:
    /** The text without leading and trailing space, tab, carriage return and newline. */
    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    /** The text without a leading UTF-8 byte order mark. */
    std::string stripBom(const std::string& text) const {
        return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
    }
};
