module;

#include <cstdint>

export module pi.support.semver_comparator;

import std;
export import pi.types.semver_version;

/**
 * Orders semantic versions (`1.2.3`, `1.2.3-beta.1`, `v1.2.3+build`) the way semver.org says: numerically by major, minor and
 * patch, a prerelease below its release, prerelease identifiers one by one (numbers below words, numbers by value, words
 * alphabetically, fewer identifiers below more); build metadata is ignored. Text that is not a version is nullopt.
 */
export class SemverComparator {
public:
    /** Negative, zero or positive like `a <=> b`; nullopt when either is not a version. */
    std::optional<int> compare(const std::string& left, const std::string& right) const {
        const auto a = parse(left);
        const auto b = parse(right);
        if (!a || !b) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (a->numbers[i] != b->numbers[i]) {
                return a->numbers[i] < b->numbers[i] ? -1 : 1;
            }
        }
        return comparePrerelease(a->prerelease, b->prerelease);
    }

    /** True for `major.minor.patch` with an optional prerelease and build, the form `npm install name@x` pins. */
    bool isExact(const std::string& text) const {
        return parse(text).has_value();
    }

private:
    std::optional<SemverVersion> parse(std::string text) const {
        if (text.starts_with("v") || text.starts_with("=")) {
            text.erase(0, 1);
        }
        if (const auto plus = text.find('+'); plus != std::string::npos) {
            text.erase(plus);
        }
        SemverVersion out;
        std::string core = text;
        if (const auto dash = text.find('-'); dash != std::string::npos) {
            core = text.substr(0, dash);
            const std::string tail = text.substr(dash + 1);
            if (tail.empty()) {
                return std::nullopt;
            }
            std::size_t start = 0;
            while (start <= tail.size()) {
                const auto dot = tail.find('.', start);
                const std::string part = tail.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                if (part.empty() || part.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-") != std::string::npos) {
                    return std::nullopt;
                }
                out.prerelease.push_back(part);
                if (dot == std::string::npos) {
                    break;
                }
                start = dot + 1;
            }
        }
        std::size_t start = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto dot = core.find('.', start);
            if ((dot == std::string::npos) != (i == 2)) {
                return std::nullopt;
            }
            const std::string part = core.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), out.numbers[i]);
            if (part.empty() || error != std::errc() || end != part.data() + part.size()) {
                return std::nullopt;
            }
            start = dot + 1;
        }
        return out;
    }

    int comparePrerelease(const std::vector<std::string>& a, const std::vector<std::string>& b) const {
        if (a.empty() != b.empty()) {
            return a.empty() ? 1 : -1;
        }
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            if (a[i] == b[i]) {
                continue;
            }
            const bool aNumber = a[i].find_first_not_of("0123456789") == std::string::npos;
            const bool bNumber = b[i].find_first_not_of("0123456789") == std::string::npos;
            if (aNumber && bNumber) {
                std::uint64_t x = 0;
                std::uint64_t y = 0;
                std::from_chars(a[i].data(), a[i].data() + a[i].size(), x);
                std::from_chars(b[i].data(), b[i].data() + b[i].size(), y);
                return x < y ? -1 : (x > y ? 1 : 0);
            }
            if (aNumber != bNumber) {
                return aNumber ? -1 : 1;
            }
            return a[i] < b[i] ? -1 : 1;
        }
        if (a.size() == b.size()) {
            return 0;
        }
        return a.size() < b.size() ? -1 : 1;
    }
};
