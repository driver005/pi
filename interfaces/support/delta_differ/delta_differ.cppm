module;

#include <cstdint>

export module pi.support.delta_differ;

import std;
export import pi.support.json_equality;
export import pi.support.utf16_text;
export import pi.types.json;

/**
 * Computes a compact operation batch that turns one JSON revision into another. Port of
 * diffRevisions in packages/chord/src/delta/diff.ts. Objects diff key by key, strings become
 * append/truncate operations when they overlap, and arrays are aligned in this order: trimmed
 * common prefix and suffix, positional matches (equal lengths), an in-order subsequence of equal
 * scalars, a longest in-order run of scalar anchors, a longest common subsequence of equal
 * elements, a single-element recursion, and finally one splice. A permutation of scalars becomes
 * one `m`. A diff of more than 4096 operations, or a large one that is not smaller than the new
 * value, collapses into a root replacement.
 *
 * The TypeScript code also aligns elements by object identity (`===`) when the caller reuses
 * untouched containers between revisions. JSON values carry no identity here, so containers are
 * never identical and only scalars anchor; the output equals the TypeScript output for revisions
 * that went through a serialisation boundary (the golden vectors). apply(before, diff) == after
 * holds either way.
 */
export class DeltaDiffer {
public:
    static constexpr std::size_t kMaxOperations = 4096;
    static constexpr std::size_t kOverlapScan = 65536;
    static constexpr std::size_t kLargeDeltaCost = 65536;
    static constexpr std::size_t kMaxIdentityCandidates = 200000;
    static constexpr std::size_t kMaxSemanticCells = 65536;

    Json diff(const Json& before, const Json& after) const {
        Json ops = Json::array();
        if (!diffValue(before, after, Json::array(), ops)) {
            return Json::array({Json::array({"r", after})});
        }
        if (ops.empty() || ops[0][0] == "r") {
            return ops;
        }
        std::size_t deltaCost = 2;
        for (const Json& op : ops) {
            deltaCost += operationCost(op) + 1;
        }
        if (deltaCost < kLargeDeltaCost) {
            return ops;
        }
        return deltaCost >= jsonCost(after) + 6 ? Json::array({Json::array({"r", after})}) : ops;
    }

private:
    bool diffValue(const Json& before, const Json& after, const Json& path, Json& ops) const {
        if (sameIdentity(before, after)) {
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

    bool diffObject(const Json& before, const Json& after, const Json& path, Json& ops) const {
        if (hasReservedKey(before) || hasReservedKey(after)) {
            return m_equality.equal(before, after) || emitSet(path, after, ops);
        }
        for (const auto& entry : after.items()) {
            const bool ok = before.contains(entry.key()) ? diffValue(before.at(entry.key()), entry.value(), child(path, entry.key()), ops)
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

    bool hasReservedKey(const Json& object) const {
        for (const auto& entry : object.items()) {
            if (entry.key() == "__proto__" || entry.key() == "constructor" || entry.key() == "prototype") {
                return true;
            }
        }
        return false;
    }

    bool diffString(const std::string& before, const std::string& after, const Json& path, Json& ops) const {
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

    bool diffArray(const Json& before, const Json& after, const Json& path, Json& ops) const {
        if (m_equality.equal(before, after)) {
            return true;
        }
        if (before.size() == after.size() && before.size() > 1 && !m_equality.equal(before.front(), after.front()) &&
            !m_equality.equal(before.back(), after.back())) {
            if (const auto order = permutation(before, after)) {
                return emit(ops, Json::array({"m", path, *order}));
            }
        }
        return diffRegion(before, after, path, ops, 0, before.size(), 0, after.size(), 0);
    }

    bool diffRegion(const Json& before, const Json& after, const Json& path, Json& ops, std::size_t beforeStart, std::size_t beforeEnd, std::size_t afterStart, std::size_t afterEnd, std::size_t outputStart) const {
        while (beforeStart < beforeEnd && afterStart < afterEnd && m_equality.equal(before[beforeStart], after[afterStart])) {
            ++beforeStart;
            ++afterStart;
            ++outputStart;
        }
        while (beforeStart < beforeEnd && afterStart < afterEnd && m_equality.equal(before[beforeEnd - 1], after[afterEnd - 1])) {
            --beforeEnd;
            --afterEnd;
        }
        const std::size_t beforeCount = beforeEnd - beforeStart;
        const std::size_t afterCount = afterEnd - afterStart;
        if (beforeCount == 0 && afterCount == 0) {
            return true;
        }
        if (beforeCount == 0 || afterCount == 0) {
            return emitSplice(after, path, ops, outputStart, beforeCount, afterStart, afterEnd);
        }
        if (beforeCount == afterCount) {
            std::vector<std::pair<std::size_t, std::size_t>> positional;
            for (std::size_t offset = 0; offset < beforeCount; ++offset) {
                if (m_equality.equal(before[beforeStart + offset], after[afterStart + offset])) {
                    positional.emplace_back(beforeStart + offset, afterStart + offset);
                }
            }
            if (!positional.empty()) {
                return processMatches(before, after, path, ops, beforeStart, beforeEnd, afterStart, afterEnd, outputStart, positional);
            }
        }
        const auto subsequence = scalarSubsequence(before, beforeStart, beforeEnd, after, afterStart, afterEnd);
        if (!subsequence.empty()) {
            return processMatches(before, after, path, ops, beforeStart, beforeEnd, afterStart, afterEnd, outputStart, subsequence);
        }
        const auto anchors = scalarAnchors(before, beforeStart, beforeEnd, after, afterStart, afterEnd);
        if (!anchors.empty()) {
            return processMatches(before, after, path, ops, beforeStart, beforeEnd, afterStart, afterEnd, outputStart, anchors);
        }
        const auto semantic = commonSubsequence(before, beforeStart, beforeEnd, after, afterStart, afterEnd);
        if (!semantic.empty()) {
            return processMatches(before, after, path, ops, beforeStart, beforeEnd, afterStart, afterEnd, outputStart, semantic);
        }
        if (beforeCount == 1 && afterCount == 1) {
            return diffValue(before[beforeStart], after[afterStart], child(path, outputStart), ops);
        }
        return emitSplice(after, path, ops, outputStart, beforeCount, afterStart, afterEnd);
    }

    bool processMatches(const Json& before, const Json& after, const Json& path, Json& ops, std::size_t beforeStart, std::size_t beforeEnd, std::size_t afterStart, std::size_t afterEnd, std::size_t outputStart, const std::vector<std::pair<std::size_t, std::size_t>>& matches) const {
        std::size_t beforeAt = beforeStart;
        std::size_t afterAt = afterStart;
        std::size_t outputAt = outputStart;
        for (const auto& [beforeMatch, afterMatch] : matches) {
            if (!diffRegion(before, after, path, ops, beforeAt, beforeMatch, afterAt, afterMatch, outputAt)) {
                return false;
            }
            outputAt += afterMatch - afterAt;
            if (!m_equality.equal(before[beforeMatch], after[afterMatch]) &&
                !diffValue(before[beforeMatch], after[afterMatch], child(path, outputAt), ops)) {
                return false;
            }
            outputAt += 1;
            beforeAt = beforeMatch + 1;
            afterAt = afterMatch + 1;
        }
        return diffRegion(before, after, path, ops, beforeAt, beforeEnd, afterAt, afterEnd, outputAt);
    }

    /** Scalars of the shorter side found in order in the longer side; empty when one is missing. */
    std::vector<std::pair<std::size_t, std::size_t>> scalarSubsequence(const Json& before, std::size_t beforeStart, std::size_t beforeEnd, const Json& after, std::size_t afterStart, std::size_t afterEnd) const {
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        const std::size_t beforeCount = beforeEnd - beforeStart;
        const std::size_t afterCount = afterEnd - afterStart;
        if (afterCount < beforeCount) {
            std::size_t beforeIndex = beforeStart;
            for (std::size_t afterIndex = afterStart; afterIndex < afterEnd; ++afterIndex) {
                while (beforeIndex < beforeEnd && !sameIdentity(before[beforeIndex], after[afterIndex])) {
                    ++beforeIndex;
                }
                if (beforeIndex == beforeEnd) {
                    return {};
                }
                matches.emplace_back(beforeIndex++, afterIndex);
            }
            return matches;
        }
        if (beforeCount < afterCount) {
            std::size_t afterIndex = afterStart;
            for (std::size_t beforeIndex = beforeStart; beforeIndex < beforeEnd; ++beforeIndex) {
                while (afterIndex < afterEnd && !sameIdentity(before[beforeIndex], after[afterIndex])) {
                    ++afterIndex;
                }
                if (afterIndex == afterEnd) {
                    return {};
                }
                matches.emplace_back(beforeIndex, afterIndex++);
            }
            return matches;
        }
        return {};
    }

    /** Longest in-order run of equal scalars (patience-style, over candidate pairs). */
    std::vector<std::pair<std::size_t, std::size_t>> scalarAnchors(const Json& before, std::size_t beforeStart, std::size_t beforeEnd, const Json& after, std::size_t afterStart, std::size_t afterEnd) const {
        std::map<std::string, std::vector<std::size_t>> positions;
        for (std::size_t index = beforeStart; index < beforeEnd; ++index) {
            if (const auto key = scalarKey(before[index])) {
                positions[*key].push_back(index);
            }
        }
        std::size_t candidateCount = 0;
        for (std::size_t index = afterStart; index < afterEnd; ++index) {
            const auto key = scalarKey(after[index]);
            const auto found = key ? positions.find(*key) : positions.end();
            candidateCount += found == positions.end() ? 0 : found->second.size();
            if (candidateCount > kMaxIdentityCandidates) {
                return greedyAnchors(positions, after, afterStart, afterEnd);
            }
        }
        if (candidateCount == 0) {
            return {};
        }
        std::vector<std::size_t> candidateBefore;
        std::vector<std::size_t> candidateAfter;
        std::vector<long long> candidatePrevious;
        std::vector<long long> tails;
        std::vector<std::size_t> tailValues;
        for (std::size_t afterIndex = afterStart; afterIndex < afterEnd; ++afterIndex) {
            const auto key = scalarKey(after[afterIndex]);
            const auto found = key ? positions.find(*key) : positions.end();
            if (found == positions.end()) {
                continue;
            }
            for (std::size_t index = found->second.size(); index > 0; --index) {
                const std::size_t beforeIndex = found->second[index - 1];
                const std::size_t at = static_cast<std::size_t>(std::lower_bound(tailValues.begin(), tailValues.end(), beforeIndex) - tailValues.begin());
                const long long candidateIndex = static_cast<long long>(candidateBefore.size());
                candidateBefore.push_back(beforeIndex);
                candidateAfter.push_back(afterIndex);
                candidatePrevious.push_back(at == 0 ? -1 : tails[at - 1]);
                if (at == tails.size()) {
                    tails.push_back(candidateIndex);
                    tailValues.push_back(beforeIndex);
                } else {
                    tails[at] = candidateIndex;
                    tailValues[at] = beforeIndex;
                }
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        long long candidateIndex = tails.empty() ? -1 : tails.back();
        while (candidateIndex >= 0) {
            const std::size_t at = static_cast<std::size_t>(candidateIndex);
            matches.emplace_back(candidateBefore[at], candidateAfter[at]);
            candidateIndex = candidatePrevious[at];
        }
        std::reverse(matches.begin(), matches.end());
        return matches;
    }

    std::vector<std::pair<std::size_t, std::size_t>> greedyAnchors(const std::map<std::string, std::vector<std::size_t>>& positions, const Json& after, std::size_t afterStart, std::size_t afterEnd) const {
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        std::size_t next = 0;
        for (std::size_t afterIndex = afterStart; afterIndex < afterEnd; ++afterIndex) {
            const auto key = scalarKey(after[afterIndex]);
            const auto found = key ? positions.find(*key) : positions.end();
            if (found == positions.end()) {
                continue;
            }
            const auto at = std::lower_bound(found->second.begin(), found->second.end(), next);
            if (at == found->second.end()) {
                continue;
            }
            matches.emplace_back(*at, afterIndex);
            next = *at + 1;
        }
        return matches;
    }

    /** Longest common subsequence of equal elements, for regions small enough to tabulate. */
    std::vector<std::pair<std::size_t, std::size_t>> commonSubsequence(const Json& before, std::size_t beforeStart, std::size_t beforeEnd, const Json& after, std::size_t afterStart, std::size_t afterEnd) const {
        const std::size_t rows = beforeEnd - beforeStart;
        const std::size_t columns = afterEnd - afterStart;
        if (rows == 0 || columns == 0 || rows * columns > kMaxSemanticCells) {
            return {};
        }
        const std::size_t width = columns + 1;
        std::vector<std::uint32_t> lengths((rows + 1) * width, 0);
        for (std::size_t left = rows; left > 0; --left) {
            for (std::size_t right = columns; right > 0; --right) {
                const std::size_t at = (left - 1) * width + (right - 1);
                lengths[at] = m_equality.equal(before[beforeStart + left - 1], after[afterStart + right - 1])
                                  ? lengths[left * width + right] + 1
                                  : std::max(lengths[left * width + right - 1], lengths[(left - 1) * width + right]);
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        std::size_t left = 0;
        std::size_t right = 0;
        while (left < rows && right < columns) {
            if (m_equality.equal(before[beforeStart + left], after[afterStart + right]) &&
                lengths[left * width + right] == lengths[(left + 1) * width + right + 1] + 1) {
                matches.emplace_back(beforeStart + left, afterStart + right);
                ++left;
                ++right;
            } else if (lengths[(left + 1) * width + right] >= lengths[left * width + right + 1]) {
                ++left;
            } else {
                ++right;
            }
        }
        return matches;
    }

    bool emitSplice(const Json& after, const Json& path, Json& ops, std::size_t at, std::size_t removed, std::size_t afterStart, std::size_t afterEnd) const {
        Json items = Json::array();
        for (std::size_t i = afterStart; i < afterEnd; ++i) {
            items.push_back(after[i]);
        }
        return emit(ops, Json::array({"p", path, at, removed, items}));
    }

    bool emit(Json& ops, Json op) const {
        if (ops.size() >= kMaxOperations) {
            return false;
        }
        ops.push_back(std::move(op));
        return true;
    }

    bool emitSet(const Json& path, const Json& value, Json& ops) const {
        if (path.empty()) {
            return emit(ops, Json::array({"r", value}));
        }
        return emit(ops, Json::array({"s", path, value}));
    }

    /** Scalars compare by value; containers have no identity here, so they are never identical. */
    bool sameIdentity(const Json& left, const Json& right) const {
        return !left.is_structured() && !right.is_structured() && left == right;
    }

    std::optional<std::string> scalarKey(const Json& value) const {
        if (value.is_structured()) {
            return std::nullopt;
        }
        if (value.is_number()) {
            return "n" + std::to_string(value.get<double>());
        }
        return value.dump();
    }

    /** `new[i] = old[order[i]]` when `after` holds exactly the scalars of `before`. */
    std::optional<Json> permutation(const Json& before, const Json& after) const {
        if (before.size() != after.size()) {
            return std::nullopt;
        }
        std::map<std::string, std::vector<std::size_t>> positions;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (const auto key = scalarKey(before[i])) {
                positions[*key].push_back(i);
            }
        }
        std::map<std::string, std::size_t> used;
        Json order = Json::array();
        for (const Json& item : after) {
            const auto key = scalarKey(item);
            const auto found = key ? positions.find(*key) : positions.end();
            if (found == positions.end() || used[*key] == found->second.size()) {
                return std::nullopt;
            }
            order.push_back(found->second[used[*key]++]);
        }
        return order;
    }

    std::size_t overlap(const std::string& before, const std::string& after) const {
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

    std::size_t jsonCost(const Json& value) const {
        if (value.is_null()) {
            return 4;
        }
        if (value.is_string()) {
            return m_text.length(value.get_ref<const std::string&>()) + 2;
        }
        if (value.is_number()) {
            return value.dump().size();
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? 4 : 5;
        }
        std::size_t cost = 2;
        std::size_t index = 0;
        if (value.is_array()) {
            for (const Json& item : value) {
                cost += jsonCost(item) + (index++ == 0 ? 0 : 1);
            }
            return cost;
        }
        for (const auto& entry : value.items()) {
            cost += m_text.length(entry.key()) + 3 + jsonCost(entry.value()) + (index++ == 0 ? 0 : 1);
        }
        return cost;
    }

    std::size_t pathCost(const Json& path) const {
        std::size_t cost = 2;
        std::size_t index = 0;
        for (const Json& segment : path) {
            cost += (segment.is_string() ? m_text.length(segment.get_ref<const std::string&>()) + 2 : segment.dump().size()) + (index++ == 0 ? 0 : 1);
        }
        return cost;
    }

    std::size_t operationCost(const Json& op) const {
        const std::string& verb = op[0].get_ref<const std::string&>();
        if (verb == "r") {
            return 6 + jsonCost(op[1]);
        }
        if (verb == "s") {
            return 7 + pathCost(op[1]) + jsonCost(op[2]);
        }
        if (verb == "d") {
            return 6 + pathCost(op[1]);
        }
        if (verb == "a") {
            return 7 + pathCost(op[1]) + m_text.length(op[2].get_ref<const std::string&>()) + 2;
        }
        if (verb == "t") {
            return 7 + pathCost(op[1]) + op[2].dump().size();
        }
        if (verb == "p") {
            return 10 + pathCost(op[1]) + op[2].dump().size() + op[3].dump().size() + jsonCost(op[4]);
        }
        return 7 + pathCost(op[1]) + jsonCost(op[2]);
    }

    Json child(const Json& path, const Json& segment) const {
        Json out = path;
        out.push_back(segment);
        return out;
    }

    Utf16Text m_text;
    JsonEquality m_equality;
};
