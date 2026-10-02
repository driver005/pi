#pragma once

#include <string>
#include <vector>

#include "interfaces/types/json/json.h"
#include "interfaces/types/result/result.h"

/**
 * JSON Schema subset validator with lenient argument coercion for model-produced tool calls.
 * Port of packages/ai/src/utils/validation.ts. Supported keywords: type, enum, const,
 * properties, required, additionalProperties, items, minItems, maxItems, minLength, maxLength,
 * minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf, allOf, anyOf, oneOf, not,
 * $ref into #/$defs or #/definitions. Other keywords (format, pattern, ...) are ignored.
 */
class SchemaValidator {
public:
    /**
     * Coerces `args` toward `schema` (strings to numbers, null to defaults, ...) and validates.
     * On failure the Error message lists one "  - path: problem" line per issue.
     */
    Result<Json> validateArguments(const Json& schema, const Json& args) const;

    /** True when `value` satisfies `schema` without coercion. */
    bool matches(const Json& schema, const Json& value) const;

    /** All validation problems as "path: message" strings (empty when valid). */
    std::vector<std::string> issues(const Json& schema, const Json& value) const;

private:
    void check(const Json& root, const Json& schema, const Json& value, const std::string& path,
               std::vector<std::string>& out) const;
    void checkType(const Json& schema, const Json& value, const std::string& path,
                   std::vector<std::string>& out) const;
    void checkObject(const Json& root, const Json& schema, const Json& value,
                     const std::string& path, std::vector<std::string>& out) const;
    void checkArray(const Json& root, const Json& schema, const Json& value,
                    const std::string& path, std::vector<std::string>& out) const;
    void checkNumber(const Json& schema, const Json& value, const std::string& path,
                     std::vector<std::string>& out) const;
    void checkString(const Json& schema, const Json& value, const std::string& path,
                     std::vector<std::string>& out) const;
    void checkCombinators(const Json& root, const Json& schema, const Json& value,
                          const std::string& path, std::vector<std::string>& out) const;
    const Json& resolve(const Json& root, const Json& schema) const;
    std::string join(const std::string& path, const std::string& key) const;
    std::vector<std::string> schemaTypes(const Json& schema) const;
    bool matchesJsonType(const Json& value, const std::string& type) const;
    std::size_t utf8Length(const std::string& text) const;

    Json coerce(const Json& root, const Json& value, const Json& schema) const;
    Json coercePrimitive(const Json& value, const std::string& type) const;
    Json coerceUnion(const Json& root, const Json& value, const Json& schemas) const;
    void coerceObject(const Json& root, Json& value, const Json& schema) const;
    void coerceArray(const Json& root, Json& value, const Json& schema) const;
    void normalizeOptionalNulls(const Json& root, Json& value, const Json& schema) const;
};
