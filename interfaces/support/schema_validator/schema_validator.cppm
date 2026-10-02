module;
#include <nlohmann/json.hpp>

#include <cstdlib>

export module pi.support.schema_validator;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * JSON Schema subset validator with lenient argument coercion for model-produced tool calls.
 * Port of packages/ai/src/utils/validation.ts. Supported keywords: type, enum, const,
 * properties, required, additionalProperties, items, minItems, maxItems, minLength, maxLength,
 * minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf, allOf, anyOf, oneOf, not,
 * $ref into #/$defs or #/definitions. Other keywords (format, pattern, ...) are ignored.
 */
export class SchemaValidator {
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

Result<Json> SchemaValidator::validateArguments(const Json& schema, const Json& args) const {
    Json value = args;
    normalizeOptionalNulls(schema, value, schema);
    value = coerce(schema, value, schema);
    const std::vector<std::string> problems = issues(schema, value);
    if (problems.empty()) {
        return value;
    }
    std::string message;
    for (const std::string& problem : problems) {
        message += "  - " + problem + "\n";
    }
    message.pop_back();
    return std::unexpected(Error{"validation_failed", message});
}

bool SchemaValidator::matches(const Json& schema, const Json& value) const {
    return issues(schema, value).empty();
}

std::vector<std::string> SchemaValidator::issues(const Json& schema, const Json& value) const {
    std::vector<std::string> out;
    check(schema, schema, value, "", out);
    return out;
}

std::string SchemaValidator::join(const std::string& path, const std::string& key) const {
    return path.empty() ? key : path + "." + key;
}

const Json& SchemaValidator::resolve(const Json& root, const Json& schema) const {
    const Json* current = &schema;
    for (int hops = 0; hops < 32 && current->is_object() && current->contains("$ref"); ++hops) {
        const Json& ref = (*current)["$ref"];
        if (!ref.is_string()) {
            break;
        }
        const std::string pointer = ref.get<std::string>();
        if (pointer.rfind("#/", 0) != 0) {
            break;
        }
        const Json::json_pointer target(pointer.substr(1));
        if (!root.contains(target)) {
            break;
        }
        current = &root.at(target);
    }
    return *current;
}

std::vector<std::string> SchemaValidator::schemaTypes(const Json& schema) const {
    std::vector<std::string> types;
    if (!schema.is_object() || !schema.contains("type")) {
        return types;
    }
    const Json& type = schema["type"];
    if (type.is_string()) {
        types.push_back(type.get<std::string>());
    } else if (type.is_array()) {
        for (const Json& entry : type) {
            if (entry.is_string()) {
                types.push_back(entry.get<std::string>());
            }
        }
    }
    return types;
}

bool SchemaValidator::matchesJsonType(const Json& value, const std::string& type) const {
    if (type == "number") {
        return value.is_number();
    }
    if (type == "integer") {
        if (value.is_number_integer()) {
            return true;
        }
        return value.is_number_float() && std::floor(value.get<double>()) == value.get<double>();
    }
    if (type == "boolean") {
        return value.is_boolean();
    }
    if (type == "string") {
        return value.is_string();
    }
    if (type == "null") {
        return value.is_null();
    }
    if (type == "array") {
        return value.is_array();
    }
    if (type == "object") {
        return value.is_object();
    }
    return false;
}

std::size_t SchemaValidator::utf8Length(const std::string& text) const {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

void SchemaValidator::check(const Json& root, const Json& rawSchema, const Json& value,
                            const std::string& path, std::vector<std::string>& out) const {
    const Json& schema = resolve(root, rawSchema);
    if (schema.is_boolean()) {
        if (!schema.get<bool>()) {
            out.push_back((path.empty() ? "root" : path) + ": Expected never");
        }
        return;
    }
    if (!schema.is_object()) {
        return;
    }
    checkType(schema, value, path, out);
    if (schema.contains("enum") && schema["enum"].is_array()) {
        const Json& options = schema["enum"];
        if (std::find(options.begin(), options.end(), value) == options.end()) {
            out.push_back((path.empty() ? "root" : path) + ": Expected value in enum");
        }
    }
    if (schema.contains("const") && schema["const"] != value) {
        out.push_back((path.empty() ? "root" : path) + ": Expected const value");
    }
    checkObject(root, schema, value, path, out);
    checkArray(root, schema, value, path, out);
    checkNumber(schema, value, path, out);
    checkString(schema, value, path, out);
    checkCombinators(root, schema, value, path, out);
}

void SchemaValidator::checkType(const Json& schema, const Json& value, const std::string& path,
                                std::vector<std::string>& out) const {
    const std::vector<std::string> types = schemaTypes(schema);
    if (types.empty()) {
        return;
    }
    for (const std::string& type : types) {
        if (matchesJsonType(value, type)) {
            return;
        }
    }
    std::string expected;
    for (const std::string& type : types) {
        expected += (expected.empty() ? "" : " | ") + type;
    }
    out.push_back((path.empty() ? "root" : path) + ": Expected " + expected);
}

void SchemaValidator::checkObject(const Json& root, const Json& schema, const Json& value,
                                  const std::string& path, std::vector<std::string>& out) const {
    if (!value.is_object()) {
        return;
    }
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const Json& name : schema["required"]) {
            if (name.is_string() && !value.contains(name.get<std::string>())) {
                out.push_back(join(path, name.get<std::string>()) + ": Expected required property");
            }
        }
    }
    const Json empty = Json::object();
    const Json& properties =
        schema.contains("properties") && schema["properties"].is_object() ? schema["properties"] : empty;
    for (const auto& [key, child] : value.items()) {
        if (properties.contains(key)) {
            check(root, properties[key], child, join(path, key), out);
        } else if (schema.contains("additionalProperties")) {
            const Json& extra = schema["additionalProperties"];
            if (extra.is_boolean() && !extra.get<bool>()) {
                out.push_back(join(path, key) + ": Unexpected property");
            } else if (extra.is_object()) {
                check(root, extra, child, join(path, key), out);
            }
        }
    }
}

void SchemaValidator::checkArray(const Json& root, const Json& schema, const Json& value,
                                 const std::string& path, std::vector<std::string>& out) const {
    if (!value.is_array()) {
        return;
    }
    const std::string label = path.empty() ? "root" : path;
    if (schema.contains("minItems") && schema["minItems"].is_number() &&
        value.size() < schema["minItems"].get<std::size_t>()) {
        out.push_back(label + ": Expected array length >= " + schema["minItems"].dump());
    }
    if (schema.contains("maxItems") && schema["maxItems"].is_number() &&
        value.size() > schema["maxItems"].get<std::size_t>()) {
        out.push_back(label + ": Expected array length <= " + schema["maxItems"].dump());
    }
    if (!schema.contains("items")) {
        return;
    }
    const Json& items = schema["items"];
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string itemPath = join(path, std::to_string(i));
        if (items.is_array()) {
            if (i < items.size()) {
                check(root, items[i], value[i], itemPath, out);
            }
        } else {
            check(root, items, value[i], itemPath, out);
        }
    }
}

void SchemaValidator::checkNumber(const Json& schema, const Json& value, const std::string& path,
                                  std::vector<std::string>& out) const {
    if (!value.is_number()) {
        return;
    }
    const double number = value.get<double>();
    const std::string label = path.empty() ? "root" : path;
    auto bound = [&](const char* key) -> const Json* {
        return schema.contains(key) && schema[key].is_number() ? &schema[key] : nullptr;
    };
    if (const Json* limit = bound("minimum"); limit != nullptr && number < limit->get<double>()) {
        out.push_back(label + ": Expected number >= " + limit->dump());
    }
    if (const Json* limit = bound("maximum"); limit != nullptr && number > limit->get<double>()) {
        out.push_back(label + ": Expected number <= " + limit->dump());
    }
    if (const Json* limit = bound("exclusiveMinimum");
        limit != nullptr && number <= limit->get<double>()) {
        out.push_back(label + ": Expected number > " + limit->dump());
    }
    if (const Json* limit = bound("exclusiveMaximum");
        limit != nullptr && number >= limit->get<double>()) {
        out.push_back(label + ": Expected number < " + limit->dump());
    }
    if (const Json* step = bound("multipleOf"); step != nullptr && step->get<double>() != 0.0) {
        const double ratio = number / step->get<double>();
        if (std::fabs(ratio - std::round(ratio)) > 1e-9) {
            out.push_back(label + ": Expected number multiple of " + step->dump());
        }
    }
}

void SchemaValidator::checkString(const Json& schema, const Json& value, const std::string& path,
                                  std::vector<std::string>& out) const {
    if (!value.is_string()) {
        return;
    }
    const std::size_t length = utf8Length(value.get<std::string>());
    const std::string label = path.empty() ? "root" : path;
    if (schema.contains("minLength") && schema["minLength"].is_number() &&
        length < schema["minLength"].get<std::size_t>()) {
        out.push_back(label + ": Expected string length >= " + schema["minLength"].dump());
    }
    if (schema.contains("maxLength") && schema["maxLength"].is_number() &&
        length > schema["maxLength"].get<std::size_t>()) {
        out.push_back(label + ": Expected string length <= " + schema["maxLength"].dump());
    }
}

void SchemaValidator::checkCombinators(const Json& root, const Json& schema, const Json& value,
                                       const std::string& path,
                                       std::vector<std::string>& out) const {
    const std::string label = path.empty() ? "root" : path;
    auto countMatches = [&](const Json& options) {
        int matched = 0;
        for (const Json& option : options) {
            std::vector<std::string> sub;
            check(root, option, value, path, sub);
            matched += sub.empty() ? 1 : 0;
        }
        return matched;
    };
    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        for (const Json& option : schema["allOf"]) {
            check(root, option, value, path, out);
        }
    }
    if (schema.contains("anyOf") && schema["anyOf"].is_array() && countMatches(schema["anyOf"]) == 0) {
        out.push_back(label + ": Expected value matching any of the union members");
    }
    if (schema.contains("oneOf") && schema["oneOf"].is_array() && countMatches(schema["oneOf"]) != 1) {
        out.push_back(label + ": Expected value matching exactly one union member");
    }
    if (schema.contains("not")) {
        std::vector<std::string> sub;
        check(root, schema["not"], value, path, sub);
        if (sub.empty()) {
            out.push_back(label + ": Expected value not matching schema");
        }
    }
}

Json SchemaValidator::coercePrimitive(const Json& value, const std::string& type) const {
    auto numeric = [&](bool integer) -> Json {
        if (value.is_null()) {
            return 0;
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? 1 : 0;
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                char* end = nullptr;
                const double parsed = std::strtod(text.c_str() + first, &end);
                const std::string rest(end);
                const bool clean = rest.find_first_not_of(" \t\r\n") == std::string::npos;
                const bool valid = clean && std::isfinite(parsed) &&
                                   (!integer || std::floor(parsed) == parsed);
                if (valid) {
                    return integer ? Json(static_cast<std::int64_t>(parsed)) : Json(parsed);
                }
            }
        }
        return value;
    };
    if (type == "number") {
        return numeric(false);
    }
    if (type == "integer") {
        return numeric(true);
    }
    if (type == "boolean") {
        if (value.is_null()) {
            return false;
        }
        if (value.is_string() && (value == "true" || value == "false")) {
            return value == "true";
        }
        if (value.is_number() && (value.get<double>() == 1.0 || value.get<double>() == 0.0)) {
            return value.get<double>() == 1.0;
        }
        return value;
    }
    if (type == "string") {
        if (value.is_null()) {
            return "";
        }
        if (value.is_number() || value.is_boolean()) {
            return value.dump();
        }
        return value;
    }
    if (type == "null") {
        const bool nullish = (value.is_string() && value.get<std::string>().empty()) ||
                             (value.is_number() && value.get<double>() == 0.0) ||
                             (value.is_boolean() && !value.get<bool>());
        return nullish ? Json(nullptr) : value;
    }
    return value;
}

Json SchemaValidator::coerceUnion(const Json& root, const Json& value, const Json& schemas) const {
    std::vector<std::string> sub;
    for (const Json& schema : schemas) {
        sub.clear();
        check(root, schema, value, "", sub);
        if (sub.empty()) {
            return value;
        }
    }
    for (const Json& schema : schemas) {
        const Json candidate = coerce(root, value, schema);
        sub.clear();
        check(root, schema, candidate, "", sub);
        if (sub.empty()) {
            return candidate;
        }
    }
    return value;
}

void SchemaValidator::coerceObject(const Json& root, Json& value, const Json& schema) const {
    const Json empty = Json::object();
    const Json& properties =
        schema.contains("properties") && schema["properties"].is_object() ? schema["properties"] : empty;
    for (const auto& [key, propertySchema] : properties.items()) {
        if (value.contains(key)) {
            value[key] = coerce(root, value[key], propertySchema);
        }
    }
    if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object()) {
        for (auto& [key, child] : value.items()) {
            if (!properties.contains(key)) {
                child = coerce(root, child, schema["additionalProperties"]);
            }
        }
    }
}

void SchemaValidator::coerceArray(const Json& root, Json& value, const Json& schema) const {
    if (!schema.contains("items")) {
        return;
    }
    const Json& items = schema["items"];
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (items.is_array()) {
            if (i < items.size()) {
                value[i] = coerce(root, value[i], items[i]);
            }
        } else if (items.is_object()) {
            value[i] = coerce(root, value[i], items);
        }
    }
}

Json SchemaValidator::coerce(const Json& root, const Json& input, const Json& rawSchema) const {
    const Json& schema = resolve(root, rawSchema);
    if (!schema.is_object()) {
        return input;
    }
    Json next = input;
    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        for (const Json& nested : schema["allOf"]) {
            next = coerce(root, next, nested);
        }
    }
    for (const char* keyword : {"anyOf", "oneOf"}) {
        if (schema.contains(keyword) && schema[keyword].is_array()) {
            next = coerceUnion(root, next, schema[keyword]);
        }
    }
    const std::vector<std::string> types = schemaTypes(schema);
    bool unionMember = false;
    for (const std::string& type : types) {
        unionMember = unionMember || (types.size() > 1 && matchesJsonType(next, type));
    }
    if (!types.empty() && !unionMember) {
        for (const std::string& type : types) {
            const Json candidate = coercePrimitive(next, type);
            if (candidate != next) {
                next = candidate;
                break;
            }
        }
    }
    const bool wantsObject = std::find(types.begin(), types.end(), "object") != types.end();
    const bool wantsArray = std::find(types.begin(), types.end(), "array") != types.end();
    if (wantsObject && next.is_object()) {
        coerceObject(root, next, schema);
    }
    if (wantsArray && next.is_array()) {
        coerceArray(root, next, schema);
    }
    return next;
}

void SchemaValidator::normalizeOptionalNulls(const Json& root, Json& value,
                                             const Json& rawSchema) const {
    const Json& schema = resolve(root, rawSchema);
    if (!schema.is_object()) {
        return;
    }
    if (value.is_array() && schema.contains("items")) {
        const Json& items = schema["items"];
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (items.is_array()) {
                if (i < items.size()) {
                    normalizeOptionalNulls(root, value[i], items[i]);
                }
            } else {
                normalizeOptionalNulls(root, value[i], items);
            }
        }
        return;
    }
    if (!value.is_object() || !schema.contains("properties") || !schema["properties"].is_object()) {
        return;
    }
    Json required = schema.contains("required") ? schema["required"] : Json::array();
    for (const auto& [key, propertySchema] : schema["properties"].items()) {
        if (!value.contains(key)) {
            continue;
        }
        const bool isRequired = std::find(required.begin(), required.end(), key) != required.end();
        const bool hasRef = propertySchema.is_object() && propertySchema.contains("$ref");
        std::vector<std::string> nullIssues;
        check(root, propertySchema, Json(nullptr), "", nullIssues);
        if (value[key].is_null() && !isRequired && !hasRef && !nullIssues.empty()) {
            value.erase(key);
        } else {
            normalizeOptionalNulls(root, value[key], propertySchema);
        }
    }
}
