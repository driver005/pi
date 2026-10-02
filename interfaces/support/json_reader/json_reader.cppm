module;
#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.json_reader;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * Typed field access over a JSON object, used by every codec. Required fields report an Error
 * naming the full path; optional fields are lenient (absent, null or wrong type give nullopt).
 */
export class JsonReader {
public:
    explicit JsonReader(const Json& value, std::string path = "");

    bool isObject() const;
    const Json& json() const;
    bool has(const std::string& key) const;
    /** Child field or a null Json when absent. */
    const Json& get(const std::string& key) const;
    JsonReader child(const std::string& key) const;
    std::string pathOf(const std::string& key) const;

    Result<std::string> requireString(const std::string& key) const;
    Result<std::int64_t> requireInt(const std::string& key) const;
    Result<double> requireNumber(const std::string& key) const;
    Result<bool> requireBool(const std::string& key) const;
    Result<const Json*> requireArray(const std::string& key) const;
    Result<const Json*> requireObject(const std::string& key) const;

    std::optional<std::string> optString(const std::string& key) const;
    std::optional<std::int64_t> optInt(const std::string& key) const;
    std::optional<double> optNumber(const std::string& key) const;
    std::optional<bool> optBool(const std::string& key) const;

    Error missing(const std::string& key, const std::string& expected) const;

private:
    const Json& m_value;
    std::string m_path;
};

JsonReader::JsonReader(const Json& value, std::string path)
    : m_value(value), m_path(std::move(path)) {}

bool JsonReader::isObject() const {
    return m_value.is_object();
}

const Json& JsonReader::json() const {
    return m_value;
}

bool JsonReader::has(const std::string& key) const {
    return m_value.is_object() && m_value.contains(key) && !m_value[key].is_null();
}

const Json& JsonReader::get(const std::string& key) const {
    static const Json kNull;
    if (!m_value.is_object()) {
        return kNull;
    }
    const auto found = m_value.find(key);
    return found == m_value.end() ? kNull : *found;
}

JsonReader JsonReader::child(const std::string& key) const {
    return JsonReader(get(key), pathOf(key));
}

std::string JsonReader::pathOf(const std::string& key) const {
    return m_path.empty() ? key : m_path + "." + key;
}

Error JsonReader::missing(const std::string& key, const std::string& expected) const {
    return Error{"invalid_json", pathOf(key) + ": expected " + expected};
}

Result<std::string> JsonReader::requireString(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_string()) {
        return std::unexpected(missing(key, "string"));
    }
    return field.get<std::string>();
}

Result<std::int64_t> JsonReader::requireInt(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_number()) {
        return std::unexpected(missing(key, "integer"));
    }
    return field.is_number_integer() ? field.get<std::int64_t>()
                                     : static_cast<std::int64_t>(field.get<double>());
}

Result<double> JsonReader::requireNumber(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_number()) {
        return std::unexpected(missing(key, "number"));
    }
    return field.get<double>();
}

Result<bool> JsonReader::requireBool(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_boolean()) {
        return std::unexpected(missing(key, "boolean"));
    }
    return field.get<bool>();
}

Result<const Json*> JsonReader::requireArray(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_array()) {
        return std::unexpected(missing(key, "array"));
    }
    return &field;
}

Result<const Json*> JsonReader::requireObject(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_object()) {
        return std::unexpected(missing(key, "object"));
    }
    return &field;
}

std::optional<std::string> JsonReader::optString(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_string()) {
        return std::nullopt;
    }
    return field.get<std::string>();
}

std::optional<std::int64_t> JsonReader::optInt(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_number()) {
        return std::nullopt;
    }
    return field.is_number_integer() ? field.get<std::int64_t>()
                                     : static_cast<std::int64_t>(field.get<double>());
}

std::optional<double> JsonReader::optNumber(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_number()) {
        return std::nullopt;
    }
    return field.get<double>();
}

std::optional<bool> JsonReader::optBool(const std::string& key) const {
    const Json& field = get(key);
    if (!field.is_boolean()) {
        return std::nullopt;
    }
    return field.get<bool>();
}
