#pragma once

#include <string>
#include <string_view>
#include <vector>

/**
 * minimatch-style glob matching with dot files allowed: `*` and `?` stay within a path segment,
 * `**` spans segments, `[abc]`/`[a-z]`/`[!x]` classes, `{a,b}` braces, leading `!` negates.
 */
class GlobMatcher {
public:
    /** True when `path` (always '/' separated) matches `pattern`. */
    bool matches(std::string_view pattern, std::string_view path) const;

private:
    std::vector<std::string> expandBraces(const std::string& pattern) const;
    std::vector<std::string> split(std::string_view text) const;
    bool matchSegments(const std::vector<std::string>& pattern, std::size_t pi,
                       const std::vector<std::string>& path, std::size_t si) const;
    bool matchSegment(std::string_view pattern, std::string_view text) const;
    bool matchClass(std::string_view pattern, std::size_t& pi, char c, bool& matched) const;
};
