#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "interfaces/types/json/json.h"
#include "interfaces/types/result/result.h"

/**
 * Typed field access over a JSON object, used by every codec. Required fields report an Error
 * naming the full path; optional fields are lenient (absent, null or wrong type give nullopt).
 */
class JsonReader {
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
