#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "interfaces/types/json/json.h"
#include "interfaces/types/result/result.h"

/** Outcome of reading one value from possibly truncated JSON text. */
enum class PartialStatus { Ok, Incomplete, Malformed };

/**
 * Tolerant JSON reading for streamed tool-call arguments.
 * Port of packages/ai/src/utils/json-parse.ts together with the `partial-json` library
 * semantics (partial strings and numbers kept, partial literals completed, dangling keys dropped).
 */
class PartialJsonParser {
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
