export module pi.support.json_comment_stripper;

import std;

/**
 * Makes "JSON with comments" parseable: removes `//` line comments and trailing commas outside
 * string literals. Port of stripJsonComments in coding-agent utils/json.ts.
 */
export class JsonCommentStripper {
public:
    std::string strip(const std::string& input) const {
        return removeTrailingCommas(removeLineComments(input));
    }

private:
    std::size_t skipString(const std::string& text, std::size_t start) const {
        std::size_t i = start + 1;
        while (i < text.size() && text[i] != '"') {
            i += text[i] == '\\' ? 2 : 1;
        }
        return std::min(i + 1, text.size());
    }

    std::string removeLineComments(const std::string& input) const {
        std::string out;
        std::size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '"') {
                const std::size_t end = skipString(input, i);
                out.append(input, i, end - i);
                i = end;
            } else if (input[i] == '/' && i + 1 < input.size() && input[i + 1] == '/') {
                while (i < input.size() && input[i] != '\n') {
                    ++i;
                }
            } else {
                out.push_back(input[i++]);
            }
        }
        return out;
    }

    std::string removeTrailingCommas(const std::string& input) const {
        std::string out;
        std::size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '"') {
                const std::size_t end = skipString(input, i);
                out.append(input, i, end - i);
                i = end;
            } else if (input[i] == ',') {
                std::size_t j = i + 1;
                while (j < input.size() && std::isspace(static_cast<unsigned char>(input[j])) != 0) {
                    ++j;
                }
                if (j < input.size() && (input[j] == '}' || input[j] == ']')) {
                    ++i;
                } else {
                    out.push_back(input[i++]);
                }
            } else {
                out.push_back(input[i++]);
            }
        }
        return out;
    }
};
