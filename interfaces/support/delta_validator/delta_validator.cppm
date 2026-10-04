module;

#include <cmath>

export module pi.support.delta_validator;

import std;
export import pi.types.json;
export import pi.types.result;

/**
 * Shape checks for delta operations, which are Json tuples: decoded ops carry their path inline
 * (`["s", path, value]`), wire ops may use path ids, short forms and `["#", id, path]` definitions
 * (see DeltaEncoder). Paths never reach the prototype chain of a host object (`__proto__`,
 * `constructor`, `prototype`) and hold only strings and non-negative integers. Port of
 * assertValidOp / assertValidWireOp / assertSafePath in packages/chord/src/delta/index.ts. Error
 * codes: "delta_op" for malformed ops, "delta_unsafe_path" for forbidden path segments.
 */
export class DeltaValidator {
public:
    Result<void> validateOp(const Json& op) const {
        if (!op.is_array() || op.empty()) {
            return std::unexpected(malformed("op is not a tuple"));
        }
        const std::string verb = op[0].is_string() ? op[0].get<std::string>() : std::string();
        const std::size_t size = op.size();
        if (verb == "r") {
            return size == 2 ? Result<void>() : std::unexpected(malformed("r arity"));
        }
        if (verb == "s" || verb == "d") {
            if (size != (verb == "s" ? 3U : 2U)) {
                return std::unexpected(malformed(verb + " arity"));
            }
            return pathArgument(op[1], true);
        }
        if (verb == "a") {
            if (size != 3 || !op[2].is_string()) {
                return std::unexpected(malformed("a shape"));
            }
            return pathArgument(op[1], true);
        }
        if (verb == "t") {
            if (size != 3 || !naturalNumber(op[2])) {
                return std::unexpected(malformed("t shape"));
            }
            return pathArgument(op[1], true);
        }
        if (verb == "p") {
            if (size != 5) {
                return std::unexpected(malformed("p arity"));
            }
            if (auto path = pathArgument(op[1], false); !path) {
                return path;
            }
            if (!naturalNumber(op[2])) {
                return std::unexpected(malformed("p index"));
            }
            if (!naturalNumber(op[3])) {
                return std::unexpected(malformed("p remove"));
            }
            return op[4].is_array() ? Result<void>() : std::unexpected(malformed("p items"));
        }
        if (verb == "m") {
            if (size != 3) {
                return std::unexpected(malformed("m arity"));
            }
            if (auto path = pathArgument(op[1], false); !path) {
                return path;
            }
            return permutation(op[2]);
        }
        return std::unexpected(malformed("unknown op verb: " + (op[0].is_string() ? verb : op[0].dump())));
    }

    Result<void> validateWireOp(const Json& op) const {
        if (!op.is_array() || op.empty()) {
            return std::unexpected(malformed("op is not a tuple"));
        }
        const std::string verb = op[0].is_string() ? op[0].get<std::string>() : std::string();
        const std::size_t size = op.size();
        if (verb == "r") {
            return size == 2 ? Result<void>() : std::unexpected(malformed("r arity"));
        }
        if (verb == "s") {
            if (size == 3) {
                return pathReference(op[1]);
            }
            return size == 2 ? Result<void>() : std::unexpected(malformed("s arity"));
        }
        if (verb == "d") {
            if (size == 2) {
                return pathReference(op[1]);
            }
            return size == 1 ? Result<void>() : std::unexpected(malformed("d arity"));
        }
        if (verb == "a") {
            if (size == 3) {
                if (auto reference = pathReference(op[1]); !reference) {
                    return reference;
                }
                return op[2].is_string() ? Result<void>() : std::unexpected(malformed("a value"));
            }
            if (size == 2) {
                return op[1].is_string() ? Result<void>() : std::unexpected(malformed("a value"));
            }
            return std::unexpected(malformed("a arity"));
        }
        if (verb == "t") {
            if (size == 3) {
                if (auto reference = pathReference(op[1]); !reference) {
                    return reference;
                }
                return naturalNumber(op[2]) ? Result<void>() : std::unexpected(malformed("t count"));
            }
            if (size == 2) {
                return naturalNumber(op[1]) ? Result<void>() : std::unexpected(malformed("t count"));
            }
            return std::unexpected(malformed("t arity"));
        }
        if (verb == "p") {
            if (size != 5 && size != 4) {
                return std::unexpected(malformed("p arity"));
            }
            const std::size_t base = size == 5 ? 2 : 1;
            if (size == 5) {
                if (auto reference = pathReference(op[1]); !reference) {
                    return reference;
                }
            }
            if (!naturalNumber(op[base])) {
                return std::unexpected(malformed("p index"));
            }
            if (!naturalNumber(op[base + 1])) {
                return std::unexpected(malformed("p remove"));
            }
            return op[base + 2].is_array() ? Result<void>() : std::unexpected(malformed("p items"));
        }
        if (verb == "m") {
            if (size == 3) {
                if (auto reference = pathReference(op[1]); !reference) {
                    return reference;
                }
            } else if (size != 2) {
                return std::unexpected(malformed("m arity"));
            }
            return permutation(op[size - 1]);
        }
        if (verb == "#") {
            if (size != 3 || !naturalNumber(op[1]) || !op[2].is_array()) {
                return std::unexpected(malformed("# shape"));
            }
            return validatePath(op[2]);
        }
        return std::unexpected(malformed("unknown op verb: " + (op[0].is_string() ? verb : op[0].dump())));
    }

    Result<void> validatePath(const Json& path) const {
        for (const Json& segment : path) {
            if (segment.is_string()) {
                const std::string& text = segment.get_ref<const std::string&>();
                if (text == "__proto__" || text == "constructor" || text == "prototype") {
                    return std::unexpected(Error{"delta_unsafe_path", "unsafe path segment: " + text});
                }
            } else if (!naturalNumber(segment)) {
                return std::unexpected(Error{"delta_unsafe_path", "unsafe path segment: " + segment.dump()});
            }
        }
        return {};
    }

    /** True when the batch starts with a root replacement. */
    bool isBase(const Json& ops) const {
        return ops.is_array() && !ops.empty() && ops[0].is_array() && !ops[0].empty() && ops[0][0] == "r";
    }

private:
    Result<void> pathArgument(const Json& path, bool nonEmpty) const {
        if (!path.is_array()) {
            return std::unexpected(malformed("path is not an array"));
        }
        if (nonEmpty && path.empty()) {
            return std::unexpected(malformed("path is empty"));
        }
        return validatePath(path);
    }

    Result<void> pathReference(const Json& reference) const {
        if (reference.is_number()) {
            if (!naturalNumber(reference)) {
                return std::unexpected(malformed("bad path id"));
            }
            return {};
        }
        if (!reference.is_array()) {
            return std::unexpected(malformed("path is not an array"));
        }
        return validatePath(reference);
    }

    Result<void> permutation(const Json& value) const {
        if (!value.is_array()) {
            return std::unexpected(malformed("m permutation is not an array"));
        }
        std::vector<bool> seen(value.size(), false);
        for (const Json& entry : value) {
            if (!naturalNumber(entry)) {
                return std::unexpected(malformed("m permutation is not a bijection"));
            }
            const auto index = static_cast<std::size_t>(entry.get<double>());
            if (index >= seen.size() || seen[index]) {
                return std::unexpected(malformed("m permutation is not a bijection"));
            }
            seen[index] = true;
        }
        return {};
    }

    bool naturalNumber(const Json& value) const {
        if (value.is_number_integer()) {
            return value.get<std::int64_t>() >= 0;
        }
        if (value.is_number_float()) {
            const double number = value.get<double>();
            return std::isfinite(number) && std::floor(number) == number && number >= 0;
        }
        return false;
    }

    Error malformed(const std::string& message) const {
        return Error{"delta_op", message};
    }
};
