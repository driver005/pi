module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.support.delta_differ;

import std;
export import pi.support.utf16_text;
export import pi.types.json;

/**
 * Computes a compact operation batch that turns one JSON revision into another. Objects diff key
 * by key, strings become append/truncate operations when they overlap, arrays trim their common
 * prefix and suffix and splice the rest (or permute, when the same elements were reordered). A diff
 * that would need more than 4096 operations, or whose encoded form is not smaller than the new
 * value for large changes, collapses into one root replacement. The result is valid for any
 * consumer (apply(before, diff) == after) but is not necessarily the same operation sequence the
 * TypeScript implementation emits: that one also aligns array elements by object identity.
 */
export class DeltaDiffer {
public:
    static constexpr std::size_t kMaxOperations = 4096;
    static constexpr std::size_t kOverlapScan = 65536;
    static constexpr std::size_t kLargeDeltaBytes = 65536;

    Json diff(const Json& before, const Json& after) const;

private:
    bool diffValue(const Json& before, const Json& after, const Json& path, Json& ops) const;
    bool diffObject(const Json& before, const Json& after, const Json& path, Json& ops) const;
    bool diffArray(const Json& before, const Json& after, const Json& path, Json& ops) const;
    bool diffString(const std::string& before, const std::string& after, const Json& path, Json& ops) const;
    bool diffRegion(const Json& before, const Json& after, const Json& path, Json& ops, std::size_t beforeStart,
                    std::size_t beforeEnd, std::size_t afterStart, std::size_t afterEnd,
                    std::size_t outputStart) const;
    bool emit(Json& ops, Json op) const;
    bool emitSet(const Json& path, const Json& value, Json& ops) const;
    std::optional<Json> permutation(const Json& before, const Json& after) const;
    std::size_t overlap(const std::string& before, const std::string& after) const;
    Json child(const Json& path, const Json& segment) const;

    Utf16Text m_text;
};

Json DeltaDiffer::child(const Json& path, const Json& segment) const {
    Json out = path;
    out.push_back(segment);
    return out;
}

bool DeltaDiffer::emit(Json& ops, Json op) const {
    if (ops.size() >= kMaxOperations) {
        return false;
    }
    ops.push_back(std::move(op));
    return true;
}

bool DeltaDiffer::emitSet(const Json& path, const Json& value, Json& ops) const {
    if (path.empty()) {
        return emit(ops, Json::array({"r", value}));
    }
    return emit(ops, Json::array({"s", path, value}));
}

std::size_t DeltaDiffer::overlap(const std::string& before, const std::string& after) const {
    if (before.empty() || after.empty()) {
        return 0;
    }
    const std::size_t scan = std::min(before.size(), kOverlapScan);
    const std::string_view tail = std::string_view(before).substr(before.size() - scan);
    const std::size_t probes[] = {std::min<std::size_t>(64, after.size()), 1};
    for (const std::size_t head : probes) {
        const std::string_view needle = std::string_view(after).substr(0, head);
        std::size_t tried = 0;
        for (std::size_t at = tail.find(needle); at != std::string_view::npos; at = tail.find(needle, at + 1)) {
            if (++tried > 8) {
                break;
            }
            const std::size_t length = tail.size() - at;
            const std::size_t start = before.size() - length;
            if (length <= after.size() && tail.substr(at) == std::string_view(after).substr(0, length) &&
                m_text.boundary(before, start)) {
                return length;
            }
        }
        if (head == 1) {
            break;
        }
    }
    return 0;
}

bool DeltaDiffer::diffString(const std::string& before, const std::string& after, const Json& path, Json& ops) const {
    if (before == after) {
        return true;
    }
    if (after.size() > before.size() && after.compare(0, before.size(), before) == 0) {
        return emit(ops, Json::array({"a", path, after.substr(before.size())}));
    }
    const std::size_t shared = overlap(before, after);
    if (shared == 0) {
        return emit(ops, Json::array({"s", path, after}));
    }
    const std::size_t dropped = m_text.length(before) - m_text.length(std::string_view(before).substr(before.size() - shared));
    if (!emit(ops, Json::array({"t", path, dropped}))) {
        return false;
    }
    return after.size() <= shared || emit(ops, Json::array({"a", path, after.substr(shared)}));
}

bool DeltaDiffer::diffObject(const Json& before, const Json& after, const Json& path, Json& ops) const {
    for (const auto& entry : after.items()) {
        if (entry.key() == "__proto__" || entry.key() == "constructor" || entry.key() == "prototype") {
            return before == after || emitSet(path, after, ops);
        }
    }
    for (const auto& entry : before.items()) {
        if (entry.key() == "__proto__" || entry.key() == "constructor" || entry.key() == "prototype") {
            return before == after || emitSet(path, after, ops);
        }
    }
    for (const auto& entry : after.items()) {
        const bool ok = before.contains(entry.key()) ? diffValue(before[entry.key()], entry.value(), child(path, entry.key()), ops)
                                                     : emitSet(child(path, entry.key()), entry.value(), ops);
        if (!ok) {
            return false;
        }
    }
    for (const auto& entry : before.items()) {
        if (!after.contains(entry.key()) && !emit(ops, Json::array({"d", child(path, entry.key())}))) {
            return false;
        }
    }
    return true;
}

std::optional<Json> DeltaDiffer::permutation(const Json& before, const Json& after) const {
    if (before.size() != after.size()) {
        return std::nullopt;
    }
    std::map<std::string, std::vector<std::size_t>> positions;
    for (std::size_t i = 0; i < before.size(); ++i) {
        positions[before[i].dump()].push_back(i);
    }
    std::map<std::string, std::size_t> used;
    Json order = Json::array();
    for (const Json& item : after) {
        const std::string key = item.dump();
        const auto found = positions.find(key);
        if (found == positions.end() || used[key] == found->second.size()) {
            return std::nullopt;
        }
        order.push_back(found->second[used[key]++]);
    }
    return order;
}

bool DeltaDiffer::diffRegion(const Json& before, const Json& after, const Json& path, Json& ops,
                             std::size_t beforeStart, std::size_t beforeEnd, std::size_t afterStart,
                             std::size_t afterEnd, std::size_t outputStart) const {
    while (beforeStart < beforeEnd && afterStart < afterEnd && before[beforeStart] == after[afterStart]) {
        ++beforeStart;
        ++afterStart;
        ++outputStart;
    }
    while (beforeStart < beforeEnd && afterStart < afterEnd && before[beforeEnd - 1] == after[afterEnd - 1]) {
        --beforeEnd;
        --afterEnd;
    }
    const std::size_t beforeCount = beforeEnd - beforeStart;
    const std::size_t afterCount = afterEnd - afterStart;
    if (beforeCount == 0 && afterCount == 0) {
        return true;
    }
    if (beforeCount == afterCount) {
        for (std::size_t offset = 0; offset < beforeCount; ++offset) {
            if (!diffValue(before[beforeStart + offset], after[afterStart + offset], child(path, outputStart + offset), ops)) {
                return false;
            }
        }
        return true;
    }
    Json items = Json::array();
    for (std::size_t i = afterStart; i < afterEnd; ++i) {
        items.push_back(after[i]);
    }
    return emit(ops, Json::array({"p", path, outputStart, beforeCount, items}));
}

bool DeltaDiffer::diffArray(const Json& before, const Json& after, const Json& path, Json& ops) const {
    if (before == after) {
        return true;
    }
    if (before.size() == after.size() && before.size() > 1 && before.front() != after.front() &&
        before.back() != after.back()) {
        if (const auto order = permutation(before, after)) {
            return emit(ops, Json::array({"m", path, *order}));
        }
    }
    return diffRegion(before, after, path, ops, 0, before.size(), 0, after.size(), 0);
}

bool DeltaDiffer::diffValue(const Json& before, const Json& after, const Json& path, Json& ops) const {
    if (before == after) {
        return true;
    }
    if (before.is_string() && after.is_string() && !path.empty()) {
        return diffString(before.get_ref<const std::string&>(), after.get_ref<const std::string&>(), path, ops);
    }
    if (before.is_array() && after.is_array()) {
        return diffArray(before, after, path, ops);
    }
    if (before.is_object() && after.is_object()) {
        return diffObject(before, after, path, ops);
    }
    return emitSet(path, after, ops);
}

Json DeltaDiffer::diff(const Json& before, const Json& after) const {
    Json ops = Json::array();
    if (!diffValue(before, after, Json::array(), ops)) {
        return Json::array({Json::array({"r", after})});
    }
    if (ops.empty() || ops[0][0] == "r") {
        return ops;
    }
    const std::size_t delta = ops.dump().size();
    if (delta < kLargeDeltaBytes) {
        return ops;
    }
    return delta >= after.dump().size() + 6 ? Json::array({Json::array({"r", after})}) : ops;
}
