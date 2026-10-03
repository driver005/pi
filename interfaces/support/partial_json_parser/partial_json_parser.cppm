module;

#include <cstddef>
#include <cctype>

export module pi.support.partial_json_parser;

import std;
export import pi.types.json;
export import pi.types.result;

/** Outcome of reading one value from possibly truncated JSON text. */
export enum class PartialStatus { Ok, Incomplete, Malformed };

/**
 * Tolerant JSON reading for streamed tool-call arguments.
 * Port of packages/ai/src/utils/json-parse.ts together with the `partial-json` library
 * semantics (partial strings and numbers kept, partial literals completed, dangling keys dropped).
 */
export class PartialJsonParser {
public:
    /** Parses text that may be cut off anywhere. Returns {} for empty or hopeless input. */
    Json parseStreaming(std::string_view text) const;

    /** Strict parse, retrying once after repairJson. */
    Result<Json> parseWithRepair(std::string_view text) const;

    /** Escapes raw control characters in strings and doubles backslashes before bad escapes. */
    std::string repairJson(std::string_view text) const;

private:
    static constexpr int kMaxDepth = 256;

    Result<Json> parseStrict(std::string_view text) const;
    bool tryPartial(std::string_view text, Json& out) const;
    void skipBlank(std::string_view text, std::size_t& pos) const;
    PartialStatus parseValue(std::string_view text, std::size_t& pos, int depth, Json& out) const;
    PartialStatus parseString(std::string_view text, std::size_t& pos, std::string& out) const;
    PartialStatus parseObject(std::string_view text, std::size_t& pos, int depth, Json& out) const;
    PartialStatus parseArray(std::string_view text, std::size_t& pos, int depth, Json& out) const;
    PartialStatus parseLiteral(std::string_view text, std::size_t& pos, Json& out) const;
    PartialStatus parseNumber(std::string_view text, std::size_t& pos, Json& out) const;
    PartialStatus parseEscape(std::string_view text, std::size_t& pos, std::string& out) const;
    void appendUtf8(unsigned int codePoint, std::string& out) const;
    bool parseHex4(std::string_view text, std::size_t pos, unsigned int& out) const;
};

Json PartialJsonParser::parseStreaming(std::string_view text) const {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return Json::object();
    }
    if (auto strict = parseWithRepair(text); strict.has_value()) {
        return *strict;
    }
    Json out;
    if (tryPartial(text, out) && !out.is_null()) {
        return out;
    }
    if (tryPartial(repairJson(text), out) && !out.is_null()) {
        return out;
    }
    return Json::object();
}

Result<Json> PartialJsonParser::parseStrict(std::string_view text) const {
    Json value = Json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded()) {
        return std::unexpected(Error{"json_parse", "invalid JSON"});
    }
    return value;
}

Result<Json> PartialJsonParser::parseWithRepair(std::string_view text) const {
    auto first = parseStrict(text);
    if (first.has_value()) {
        return first;
    }
    const std::string repaired = repairJson(text);
    if (repaired != text) {
        return parseStrict(repaired);
    }
    return first;
}

std::string PartialJsonParser::repairJson(std::string_view text) const {
    std::string repaired;
    repaired.reserve(text.size());
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!inString) {
            repaired.push_back(c);
            inString = c == '"';
            continue;
        }
        if (c == '"') {
            repaired.push_back(c);
            inString = false;
        } else if (c == '\\') {
            const bool hasNext = i + 1 < text.size();
            const char next = hasNext ? text[i + 1] : '\0';
            unsigned int unused = 0;
            if (hasNext && next == 'u' && parseHex4(text, i + 2, unused)) {
                repaired.append(text.substr(i, 6));
                i += 5;
            } else if (hasNext && std::string_view("\"\\/bfnrt").find(next) != std::string_view::npos) {
                repaired.push_back('\\');
                repaired.push_back(next);
                i += 1;
            } else {
                repaired.append("\\\\");
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            switch (c) {
                case '\b': repaired.append("\\b"); break;
                case '\f': repaired.append("\\f"); break;
                case '\n': repaired.append("\\n"); break;
                case '\r': repaired.append("\\r"); break;
                case '\t': repaired.append("\\t"); break;
                default:
                    repaired.append("\\u00");
                    repaired.push_back(kHex[(c >> 4) & 0xF]);
                    repaired.push_back(kHex[c & 0xF]);
            }
        } else {
            repaired.push_back(c);
        }
    }
    return repaired;
}

bool PartialJsonParser::tryPartial(std::string_view text, Json& out) const {
    std::size_t pos = 0;
    const PartialStatus status = parseValue(text, pos, 0, out);
    return status == PartialStatus::Ok;
}

void PartialJsonParser::skipBlank(std::string_view text, std::size_t& pos) const {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

PartialStatus PartialJsonParser::parseValue(std::string_view text, std::size_t& pos, int depth,
                                            Json& out) const {
    if (depth > kMaxDepth) {
        return PartialStatus::Malformed;
    }
    skipBlank(text, pos);
    if (pos >= text.size()) {
        return PartialStatus::Incomplete;
    }
    const char c = text[pos];
    if (c == '"') {
        std::string value;
        const PartialStatus status = parseString(text, pos, value);
        if (status == PartialStatus::Malformed) {
            return status;
        }
        out = std::move(value);
        return PartialStatus::Ok;
    }
    if (c == '{') {
        return parseObject(text, pos, depth, out);
    }
    if (c == '[') {
        return parseArray(text, pos, depth, out);
    }
    if (c == 'n' || c == 't' || c == 'f') {
        return parseLiteral(text, pos, out);
    }
    return parseNumber(text, pos, out);
}

bool PartialJsonParser::parseHex4(std::string_view text, std::size_t pos, unsigned int& out) const {
    if (pos + 4 > text.size()) {
        return false;
    }
    unsigned int value = 0;
    for (std::size_t i = pos; i < pos + 4; ++i) {
        const char h = text[i];
        value <<= 4;
        if (h >= '0' && h <= '9') {
            value |= static_cast<unsigned int>(h - '0');
        } else if (h >= 'a' && h <= 'f') {
            value |= static_cast<unsigned int>(h - 'a' + 10);
        } else if (h >= 'A' && h <= 'F') {
            value |= static_cast<unsigned int>(h - 'A' + 10);
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

void PartialJsonParser::appendUtf8(unsigned int cp, std::string& out) const {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

PartialStatus PartialJsonParser::parseEscape(std::string_view text, std::size_t& pos,
                                             std::string& out) const {
    if (pos >= text.size()) {
        return PartialStatus::Incomplete;
    }
    const char c = text[pos];
    switch (c) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            unsigned int cp = 0;
            if (!parseHex4(text, pos + 1, cp)) {
                return pos + 5 > text.size() ? PartialStatus::Incomplete : PartialStatus::Malformed;
            }
            pos += 4;
            if (cp >= 0xD800 && cp < 0xDC00) {
                unsigned int low = 0;
                const bool pair = pos + 6 < text.size() + 0 && text[pos + 1] == '\\' &&
                                  text[pos + 2] == 'u' && parseHex4(text, pos + 3, low) &&
                                  low >= 0xDC00 && low < 0xE000;
                if (!pair) {
                    if (pos + 1 >= text.size()) {
                        return PartialStatus::Incomplete;
                    }
                    cp = 0xFFFD;
                } else {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    pos += 6;
                }
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            appendUtf8(cp, out);
            break;
        }
        default:
            return PartialStatus::Malformed;
    }
    ++pos;
    return PartialStatus::Ok;
}

PartialStatus PartialJsonParser::parseString(std::string_view text, std::size_t& pos,
                                             std::string& out) const {
    ++pos;  // opening quote
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') {
            ++pos;
            return PartialStatus::Ok;
        }
        if (c != '\\') {
            out.push_back(c);
            ++pos;
            continue;
        }
        ++pos;
        const PartialStatus status = parseEscape(text, pos, out);
        if (status == PartialStatus::Incomplete) {
            pos = text.size();
        }
        if (status != PartialStatus::Ok) {
            return status;
        }
    }
    return PartialStatus::Incomplete;
}

PartialStatus PartialJsonParser::parseLiteral(std::string_view text, std::size_t& pos,
                                              Json& out) const {
    const std::string_view rest = text.substr(pos);
    static constexpr std::string_view kNull = "null";
    static constexpr std::string_view kTrue = "true";
    static constexpr std::string_view kFalse = "false";
    const std::string_view words[3] = {kNull, kTrue, kFalse};
    for (int i = 0; i < 3; ++i) {
        const std::string_view word = words[i];
        const bool whole = rest.starts_with(word);
        const bool partial = rest.size() < word.size() && word.starts_with(rest);
        if (whole || partial) {
            pos += whole ? word.size() : rest.size();
            out = i == 0 ? Json(nullptr) : Json(i == 1);
            return PartialStatus::Ok;
        }
    }
    return PartialStatus::Malformed;
}

PartialStatus PartialJsonParser::parseNumber(std::string_view text, std::size_t& pos,
                                             Json& out) const {
    std::size_t end = pos;
    while (end < text.size() && std::string_view("0123456789+-.eE").find(text[end]) !=
                                    std::string_view::npos) {
        ++end;
    }
    if (end == pos) {
        return PartialStatus::Malformed;
    }
    std::string_view token = text.substr(pos, end - pos);
    const bool atEnd = end == text.size();
    if (atEnd) {
        while (!token.empty() && std::string_view("-+.eE").find(token.back()) != std::string_view::npos) {
            token.remove_suffix(1);
        }
        if (token.empty()) {
            return PartialStatus::Incomplete;
        }
    }
    Json value = Json::parse(token.begin(), token.end(), nullptr, false);
    if (value.is_discarded() || !value.is_number()) {
        return PartialStatus::Malformed;
    }
    out = std::move(value);
    pos = end;
    return PartialStatus::Ok;
}

PartialStatus PartialJsonParser::parseObject(std::string_view text, std::size_t& pos, int depth,
                                             Json& out) const {
    Json object = Json::object();
    ++pos;
    while (true) {
        skipBlank(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] == '}') {
            ++pos;
            break;
        }
        if (text[pos] != '"') {
            return PartialStatus::Malformed;
        }
        std::string key;
        PartialStatus status = parseString(text, pos, key);
        if (status != PartialStatus::Ok) {
            if (status == PartialStatus::Malformed) {
                return status;
            }
            break;
        }
        skipBlank(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] != ':') {
            return PartialStatus::Malformed;
        }
        ++pos;
        Json value;
        status = parseValue(text, pos, depth + 1, value);
        if (status == PartialStatus::Malformed) {
            return status;
        }
        if (status == PartialStatus::Incomplete) {
            break;
        }
        object[key] = std::move(value);
        skipBlank(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] == ',') {
            ++pos;
        } else if (text[pos] == '}') {
            ++pos;
            break;
        } else {
            return PartialStatus::Malformed;
        }
    }
    out = std::move(object);
    return PartialStatus::Ok;
}

PartialStatus PartialJsonParser::parseArray(std::string_view text, std::size_t& pos, int depth,
                                            Json& out) const {
    Json array = Json::array();
    ++pos;
    while (true) {
        skipBlank(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] == ']') {
            ++pos;
            break;
        }
        Json value;
        const PartialStatus status = parseValue(text, pos, depth + 1, value);
        if (status == PartialStatus::Malformed) {
            return status;
        }
        if (status == PartialStatus::Incomplete) {
            break;
        }
        array.push_back(std::move(value));
        skipBlank(text, pos);
        if (pos >= text.size()) {
            break;
        }
        if (text[pos] == ',') {
            ++pos;
        } else if (text[pos] == ']') {
            ++pos;
            break;
        } else {
            return PartialStatus::Malformed;
        }
    }
    out = std::move(array);
    return PartialStatus::Ok;
}
