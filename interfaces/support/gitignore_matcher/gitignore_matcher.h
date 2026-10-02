#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "interfaces/support/glob_matcher/glob_matcher.h"

/** Evaluates .gitignore-style rules (last match wins, `!` re-includes, parent dirs cascade). */
class GitignoreMatcher {
public:
    /** Parses the text of one ignore file; rules apply to paths relative to its directory. */
    explicit GitignoreMatcher(std::string_view rulesText);

    /** True when `relativePath` ('/' separated, no leading slash) is ignored. */
    bool ignores(std::string_view relativePath, bool isDirectory) const;

private:
    std::string toGlob(const std::string& body, bool anchored) const;
    bool lastRuleIgnores(std::string_view path, bool isDirectory) const;

    std::vector<std::string> m_globs;
    std::vector<bool> m_negated;
    std::vector<bool> m_directoryOnly;
    GlobMatcher m_glob;
};
