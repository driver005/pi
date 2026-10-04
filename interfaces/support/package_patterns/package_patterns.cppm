export module pi.support.package_patterns;

import std;
import pi.support.glob_matcher;

/**
 * The pattern language of package filters over absolute file paths. Plain entries include (globs match the path relative to the
 * package root, the file name or the absolute path; a SKILL.md also matches through its directory), `!pattern` excludes,
 * `+path` force-includes an exact path (beats excludes) and `-path` force-excludes one (beats everything). Port of applyPatterns
 * and applyAutoloadDisabledPatterns in core/package-manager.ts.
 */
export class PackagePatterns {
public:
    bool isOverride(const std::string& entry) const {
        return entry.starts_with("!") || entry.starts_with("+") || entry.starts_with("-");
    }

    bool hasGlob(const std::string& entry) const {
        return entry.find_first_of("*?") != std::string::npos;
    }

    /** The paths of `all` that the patterns leave on, in the order of `all`. */
    std::vector<std::string> apply(const std::vector<std::string>& all, const std::vector<std::string>& patterns, const std::string& baseDir) const {
        std::vector<std::string> includes;
        std::vector<std::string> excludes;
        std::vector<std::string> forceIncludes;
        std::vector<std::string> forceExcludes;
        for (const std::string& pattern : patterns) {
            if (pattern.starts_with("+")) {
                forceIncludes.push_back(pattern.substr(1));
            } else if (pattern.starts_with("-")) {
                forceExcludes.push_back(pattern.substr(1));
            } else if (pattern.starts_with("!")) {
                excludes.push_back(pattern.substr(1));
            } else {
                includes.push_back(pattern);
            }
        }
        std::vector<std::string> result;
        for (const std::string& path : all) {
            const bool included = includes.empty() || matchesAny(path, includes, baseDir);
            const bool excluded = !excludes.empty() && matchesAny(path, excludes, baseDir);
            const bool forced = !forceIncludes.empty() && matchesExact(path, forceIncludes, baseDir);
            const bool removed = !forceExcludes.empty() && matchesExact(path, forceExcludes, baseDir);
            if (((included && !excluded) || forced) && !removed) {
                result.push_back(path);
            }
        }
        return result;
    }

    /**
     * What a delta entry decides: for each path a pattern names, on (plain and `+`) or off (`!` and `-`). Later patterns override
     * earlier ones; paths no pattern names are absent.
     */
    std::vector<std::pair<std::string, bool>> delta(const std::vector<std::string>& all, const std::vector<std::string>& patterns, const std::string& baseDir) const {
        std::vector<std::pair<std::string, bool>> result;
        for (const std::string& pattern : patterns) {
            const bool prefixed = isOverride(pattern);
            const std::string target = prefixed ? pattern.substr(1) : pattern;
            const bool enabled = !pattern.starts_with("-") && !pattern.starts_with("!");
            const bool exact = pattern.starts_with("+") || pattern.starts_with("-");
            for (const std::string& path : all) {
                if (!(exact ? matchesExact(path, {target}, baseDir) : matchesAny(path, {target}, baseDir))) {
                    continue;
                }
                const auto at = std::ranges::find(result, path, &std::pair<std::string, bool>::first);
                if (at == result.end()) {
                    result.emplace_back(path, enabled);
                } else {
                    at->second = enabled;
                }
            }
        }
        return result;
    }

private:
    bool matchesAny(const std::string& path, const std::vector<std::string>& patterns, const std::string& baseDir) const {
        const std::vector<std::string> candidates = candidatesOf(path, baseDir, false);
        for (const std::string& pattern : patterns) {
            for (const std::string& candidate : candidates) {
                if (m_glob.matches(pattern, candidate)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool matchesExact(const std::string& path, const std::vector<std::string>& patterns, const std::string& baseDir) const {
        const std::vector<std::string> candidates = candidatesOf(path, baseDir, true);
        for (const std::string& pattern : patterns) {
            const std::string normalized = pattern.starts_with("./") ? pattern.substr(2) : pattern;
            if (std::ranges::find(candidates, normalized) != candidates.end()) {
                return true;
            }
        }
        return false;
    }

    /** The spellings of a file a pattern may name: relative path, name (not for exact entries) and absolute path; a SKILL.md also by its directory. */
    std::vector<std::string> candidatesOf(const std::string& path, const std::string& baseDir, bool exact) const {
        const std::filesystem::path file(path);
        std::vector<std::string> out{relative(file, baseDir), path};
        if (!exact) {
            out.push_back(file.filename().generic_string());
        }
        if (file.filename() == "SKILL.md") {
            const std::filesystem::path parent = file.parent_path();
            out.push_back(relative(parent, baseDir));
            out.push_back(parent.generic_string());
            if (!exact) {
                out.push_back(parent.filename().generic_string());
            }
        }
        return out;
    }

    std::string relative(const std::filesystem::path& path, const std::string& baseDir) const {
        return path.lexically_relative(baseDir).generic_string();
    }

    GlobMatcher m_glob;
};
