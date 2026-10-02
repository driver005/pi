export module pi.support.ini_parser;

import std;

/**
 * Reads INI text such as ~/.aws/credentials: `[section]` headers, `key = value` lines, comments
 * starting with # or ;, surrounding whitespace trimmed. Keys before any section are ignored.
 */
export class IniParser {
public:
    using Section = std::map<std::string, std::string>;

    std::map<std::string, Section> parse(const std::string& text) const;

private:
    std::string trim(const std::string& text) const;
};

std::string IniParser::trim(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::map<std::string, IniParser::Section> IniParser::parse(const std::string& text) const {
    std::map<std::string, Section> sections;
    std::optional<std::string> current;
    std::istringstream lines(text);
    for (std::string raw; std::getline(lines, raw);) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string::npos) {
                current = trim(line.substr(1, close - 1));
                sections[*current];
            }
            continue;
        }
        const auto equals = line.find('=');
        if (current && equals != std::string::npos) {
            sections[*current][trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
        }
    }
    return sections;
}
