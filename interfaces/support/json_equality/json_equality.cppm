export module pi.support.json_equality;

import std;
export import pi.types.json;

/** Structural JSON equality that ignores object key order (ordered_json's own `==` does not). */
export class JsonEquality {
public:
    bool equal(const Json& left, const Json& right) const {
        if (left.is_object() && right.is_object()) {
            if (left.size() != right.size()) {
                return false;
            }
            for (const auto& entry : left.items()) {
                if (!right.contains(entry.key()) || !equal(entry.value(), right.at(entry.key()))) {
                    return false;
                }
            }
            return true;
        }
        if (left.is_array() && right.is_array()) {
            if (left.size() != right.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i) {
                if (!equal(left[i], right[i])) {
                    return false;
                }
            }
            return true;
        }
        return left == right;
    }
};
