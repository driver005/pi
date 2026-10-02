export module pi.support.glob_matcher;

import std;

/**
 * minimatch-style glob matching with dot files allowed: `*` and `?` stay within a path segment,
 * `**` spans segments, `[abc]`/`[a-z]`/`[!x]` classes, `{a,b}` braces, leading `!` negates.
 */
export class GlobMatcher {
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

bool GlobMatcher::matches(std::string_view pattern, std::string_view path) const {
    if (!pattern.empty() && pattern[0] == '!') {
        return !matches(pattern.substr(1), path);
    }
    const std::vector<std::string> pathSegments = split(path);
    for (const std::string& expanded : expandBraces(std::string(pattern))) {
        if (matchSegments(split(expanded), 0, pathSegments, 0)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> GlobMatcher::split(std::string_view text) const {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == '/') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool edgeEmpty = parts[i].empty() && (i == 0 || i + 1 == parts.size());
        if (!parts[i].empty() || (!edgeEmpty && false)) {
            kept.push_back(parts[i]);
        }
    }
    return kept;
}

std::vector<std::string> GlobMatcher::expandBraces(const std::string& pattern) const {
    int depth = 0;
    std::size_t open = std::string::npos;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            ++i;
        } else if (pattern[i] == '{') {
            if (depth++ == 0) {
                open = i;
            }
        } else if (pattern[i] == '}' && depth > 0 && --depth == 0) {
            std::vector<std::string> options;
            std::string current;
            int inner = 0;
            for (std::size_t j = open + 1; j < i; ++j) {
                const char c = pattern[j];
                inner += c == '{' ? 1 : (c == '}' ? -1 : 0);
                if (c == ',' && inner == 0) {
                    options.push_back(current);
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            options.push_back(current);
            std::vector<std::string> results;
            for (const std::string& option : options) {
                const std::string joined = pattern.substr(0, open) + option + pattern.substr(i + 1);
                for (std::string& expanded : expandBraces(joined)) {
                    results.push_back(std::move(expanded));
                }
            }
            return results;
        }
    }
    return {pattern};
}

bool GlobMatcher::matchSegments(const std::vector<std::string>& pattern, std::size_t pi,
                                const std::vector<std::string>& path, std::size_t si) const {
    if (pi == pattern.size()) {
        return si == path.size();
    }
    if (pattern[pi] == "**") {
        for (std::size_t skip = si; skip <= path.size(); ++skip) {
            if (matchSegments(pattern, pi + 1, path, skip)) {
                return true;
            }
        }
        return false;
    }
    if (si == path.size() || !matchSegment(pattern[pi], path[si])) {
        return false;
    }
    return matchSegments(pattern, pi + 1, path, si + 1);
}

bool GlobMatcher::matchClass(std::string_view pattern, std::size_t& pi, char c, bool& matched) const {
    std::size_t i = pi + 1;
    bool negate = false;
    if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
        negate = true;
        ++i;
    }
    bool found = false;
    bool first = true;
    while (i < pattern.size() && (pattern[i] != ']' || first)) {
        first = false;
        char low = pattern[i];
        if (low == '\\' && i + 1 < pattern.size()) {
            low = pattern[++i];
        }
        char high = low;
        if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            high = pattern[i + 2];
            i += 2;
        }
        found = found || (c >= low && c <= high);
        ++i;
    }
    if (i >= pattern.size()) {
        return false;
    }
    matched = found != negate;
    pi = i;
    return true;
}

bool GlobMatcher::matchSegment(std::string_view pattern, std::string_view text) const {
    std::size_t pi = 0;
    std::size_t ti = 0;
    std::size_t starPi = std::string_view::npos;
    std::size_t starTi = 0;
    while (ti < text.size()) {
        bool advanced = false;
        if (pi < pattern.size()) {
            const char p = pattern[pi];
            if (p == '*') {
                starPi = pi++;
                starTi = ti;
                continue;
            }
            bool ok = false;
            std::size_t next = pi;
            if (p == '?') {
                ok = true;
            } else if (p == '[') {
                bool classMatched = false;
                if (matchClass(pattern, next, text[ti], classMatched)) {
                    ok = classMatched;
                } else {
                    ok = text[ti] == '[';
                }
            } else if (p == '\\' && pi + 1 < pattern.size()) {
                ok = pattern[pi + 1] == text[ti];
                next = pi + 1;
            } else {
                ok = p == text[ti];
            }
            if (ok) {
                pi = next + 1;
                ++ti;
                advanced = true;
            }
        }
        if (advanced) {
            continue;
        }
        if (starPi == std::string_view::npos) {
            return false;
        }
        pi = starPi + 1;
        ti = ++starTi;
    }
    while (pi < pattern.size() && pattern[pi] == '*') {
        ++pi;
    }
    return pi == pattern.size();
}
