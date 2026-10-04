module;

#include <cmath>

export module pi.support.json_value_checker;

import std;
export import pi.types.json;

/** Strict JSON: null, booleans, finite numbers, strings, arrays and objects; no byte strings. */
export class JsonValueChecker {
public:
    bool valid(const Json& value) const {
        if (value.is_discarded() || value.is_binary()) {
            return false;
        }
        if (value.is_number_float()) {
            return std::isfinite(value.get<double>());
        }
        if (value.is_array()) {
            return std::all_of(value.begin(), value.end(), [this](const Json& item) { return valid(item); });
        }
        if (value.is_object()) {
            return std::all_of(value.begin(), value.end(), [this](const Json& item) { return valid(item); });
        }
        return true;
    }
};
