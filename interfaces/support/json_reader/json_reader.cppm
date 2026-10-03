module;

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
    explicit JsonReader(const Json& value, std::string path = "")
        : m_value(value),
          m_path(std::move(path)) {}

    bool isObject() const {
        return m_value.is_object();
    }

    const Json& json() const {
        return m_value;
    }

    bool has(const std::string& key) const {
        return m_value.is_object() && m_value.contains(key) && !m_value[key].is_null();
    }

    /** Child field or a null Json when absent. */
    const Json& get(const std::string& key) const {
        static const Json kNull;
        if (!m_value.is_object()) {
            return kNull;
        }
        const auto found = m_value.find(key);
        return found == m_value.end() ? kNull : *found;
    }

    JsonReader child(const std::string& key) const {
        return JsonReader(get(key), pathOf(key));
    }

    std::string pathOf(const std::string& key) const {
        return m_path.empty() ? key : m_path + "." + key;
    }

    Result<std::string> requireString(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_string()) {
            return std::unexpected(missing(key, "string"));
        }
        return field.get<std::string>();
    }

    Result<std::int64_t> requireInt(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_number()) {
            return std::unexpected(missing(key, "integer"));
        }
        return field.is_number_integer() ? field.get<std::int64_t>()
                                         : static_cast<std::int64_t>(field.get<double>());
    }

    Result<double> requireNumber(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_number()) {
            return std::unexpected(missing(key, "number"));
        }
        return field.get<double>();
    }

    Result<bool> requireBool(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_boolean()) {
            return std::unexpected(missing(key, "boolean"));
        }
        return field.get<bool>();
    }

    Result<const Json*> requireArray(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_array()) {
            return std::unexpected(missing(key, "array"));
        }
        return &field;
    }

    Result<const Json*> requireObject(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_object()) {
            return std::unexpected(missing(key, "object"));
        }
        return &field;
    }

    std::optional<std::string> optString(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_string()) {
            return std::nullopt;
        }
        return field.get<std::string>();
    }

    std::optional<std::int64_t> optInt(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_number()) {
            return std::nullopt;
        }
        return field.is_number_integer() ? field.get<std::int64_t>()
                                         : static_cast<std::int64_t>(field.get<double>());
    }

    std::optional<double> optNumber(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_number()) {
            return std::nullopt;
        }
        return field.get<double>();
    }

    std::optional<bool> optBool(const std::string& key) const {
        const Json& field = get(key);
        if (!field.is_boolean()) {
            return std::nullopt;
        }
        return field.get<bool>();
    }

    Error missing(const std::string& key, const std::string& expected) const {
        return Error{"invalid_json", pathOf(key) + ": expected " + expected};
    }

private:
    const Json& m_value;
    std::string m_path;
};
