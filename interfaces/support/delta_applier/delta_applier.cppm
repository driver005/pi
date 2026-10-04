module;

#include <cmath>

export module pi.support.delta_applier;

import std;
export import pi.support.delta_validator;
export import pi.support.utf16_text;
export import pi.types.json;
export import pi.types.result;

/**
 * Applies decoded delta operations to a JSON value (copied, never aliased). `r` replaces the whole
 * value; `s`/`d`/`a`/`t` address a child of a container; `p` splices and `m` permutes an array, the
 * root included. Array writes may only hit an existing index or append exactly one past the end so
 * the value stays a faithful JSON array; `t` counts UTF-16 code units like the JavaScript original.
 * Error codes: "delta_op" (malformed op), "delta_unsafe_path", "delta_path" (unresolvable path).
 * Port of apply() in packages/chord/src/delta/index.ts.
 */
export class DeltaApplier {
public:
    Result<Json> apply(Json target, const Json& ops) const {
        if (!ops.is_array()) {
            return std::unexpected(Error{"delta_op", "ops is not an array"});
        }
        Json root = std::move(target);
        for (const Json& op : ops) {
            if (auto applied = applyOp(root, op); !applied) {
                return std::unexpected(applied.error());
            }
        }
        return root;
    }

private:
    Result<void> applyOp(Json& root, const Json& op) const {
        if (auto valid = m_validator.validateOp(op); !valid) {
            return valid;
        }
        const std::string verb = op[0].get<std::string>();
        if (verb == "r") {
            root = op[1];
            return {};
        }
        if (verb == "p") {
            return applySplice(root, op);
        }
        if (verb == "m") {
            return applyPermutation(root, op);
        }
        return applyChild(root, op);
    }

    Result<void> applySplice(Json& root, const Json& op) const {
        auto target = resolve(root, op[1], op[1].size());
        if (!target || !(*target)->is_array()) {
            return std::unexpected(pathError(op[1]));
        }
        Json::array_t& array = (*target)->get_ref<Json::array_t&>();
        const auto start = std::min(static_cast<std::size_t>(op[2].get<double>()), array.size());
        const auto remove = std::min(static_cast<std::size_t>(op[3].get<double>()), array.size() - start);
        array.erase(array.begin() + static_cast<std::ptrdiff_t>(start),
                    array.begin() + static_cast<std::ptrdiff_t>(start + remove));
        const Json& items = op[4];
        array.insert(array.begin() + static_cast<std::ptrdiff_t>(start), items.begin(), items.end());
        return {};
    }

    Result<void> applyPermutation(Json& root, const Json& op) const {
        auto target = resolve(root, op[1], op[1].size());
        if (!target || !(*target)->is_array() || (*target)->size() != op[2].size()) {
            return std::unexpected(pathError(op[1]));
        }
        Json::array_t& array = (*target)->get_ref<Json::array_t&>();
        const Json::array_t previous = array;
        for (std::size_t i = 0; i < op[2].size(); ++i) {
            array[i] = previous[static_cast<std::size_t>(op[2][i].get<double>())];
        }
        return {};
    }

    Result<void> applyChild(Json& root, const Json& op) const {
        const Json& path = op[1];
        auto parent = resolve(root, path, path.size() - 1);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        if (!(*parent)->is_structured()) {
            return std::unexpected(pathError(path));
        }
        Json& container = **parent;
        if (container.is_array()) {
            return applyToArrayElement(container, op, path);
        }
        const std::string key = keyOf(path.back());
        if (op[0] == "s") {
            container[key] = op[2];
            return {};
        }
        if (op[0] == "d") {
            container.erase(key);
            return {};
        }
        if (!container.contains(key)) {
            return std::unexpected(pathError(path));
        }
        return editString(container[key], op, path);
    }

    Result<void> applyToArrayElement(Json& array, const Json& op, const Json& path) const {
        const Json& segment = path.back();
        if (!segment.is_number()) {
            return std::unexpected(unsafe(segment));
        }
        const auto index = static_cast<std::size_t>(segment.get<double>());
        if (index > array.size()) {
            return std::unexpected(unsafe(segment));
        }
        if (op[0] == "s") {
            if (index == array.size()) {
                array.push_back(op[2]);
            } else {
                array[index] = op[2];
            }
            return {};
        }
        if (index >= array.size()) {
            return std::unexpected(pathError(path));
        }
        if (op[0] == "d") {
            array.erase(index);
            return {};
        }
        return editString(array[index], op, path);
    }

    Result<void> editString(Json& target, const Json& op, const Json& path) const {
        if (!target.is_string()) {
            return std::unexpected(pathError(path));
        }
        std::string& text = target.get_ref<std::string&>();
        if (op[0] == "a") {
            text += op[2].get_ref<const std::string&>();
            return {};
        }
        const auto offset = m_text.byteOffset(text, static_cast<std::size_t>(op[2].get<double>()));
        if (!offset) {
            text.clear();
            return {};
        }
        text.erase(0, *offset);
        return {};
    }

    Result<Json*> resolve(Json& root, const Json& path, std::size_t length) const {
        Json* node = &root;
        for (std::size_t i = 0; i < length; ++i) {
            auto next = step(*node, path[i], path);
            if (!next) {
                return next;
            }
            node = *next;
        }
        return node;
    }

    Result<Json*> step(Json& node, const Json& segment, const Json& path) const {
        if (node.is_array()) {
            if (!segment.is_number()) {
                return std::unexpected(unsafe(segment));
            }
            const auto index = static_cast<std::size_t>(segment.get<double>());
            if (index >= node.size()) {
                return std::unexpected(pathError(path));
            }
            return &node[index];
        }
        if (!node.is_object()) {
            return std::unexpected(pathError(path));
        }
        const std::string key = keyOf(segment);
        if (!node.contains(key)) {
            return std::unexpected(pathError(path));
        }
        return &node[key];
    }

    std::string keyOf(const Json& segment) const {
        return segment.is_string() ? segment.get<std::string>() : std::to_string(segment.get<std::int64_t>());
    }

    Error pathError(const Json& path) const {
        return Error{"delta_path", "unresolvable path: " + path.dump()};
    }

    Error unsafe(const Json& segment) const {
        return Error{"delta_unsafe_path", "unsafe path segment: " + segment.dump()};
    }

    DeltaValidator m_validator;
    Utf16Text m_text;
};
