export module pi.support.gitignore_matcher;

import std;
export import pi.support.glob_matcher;

/** Evaluates .gitignore-style rules (last match wins, `!` re-includes, parent dirs cascade). */
export class GitignoreMatcher {
public:
    /** Parses the text of one ignore file; rules apply to paths relative to its directory. */
    explicit GitignoreMatcher(std::string_view rulesText) {
        std::size_t start = 0;
        while (start <= rulesText.size()) {
            std::size_t end = rulesText.find('\n', start);
            end = end == std::string_view::npos ? rulesText.size() : end;
            std::string line(rulesText.substr(start, end - start));
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const bool negated = line[0] == '!';
            if (negated || line.rfind("\\!", 0) == 0 || line.rfind("\\#", 0) == 0) {
                line.erase(0, 1);
            }
            const bool directoryOnly = !line.empty() && line.back() == '/';
            if (directoryOnly) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            const bool anchored = line.find('/') != std::string::npos;
            if (line[0] == '/') {
                line.erase(0, 1);
            }
            m_globs.push_back(toGlob(line, anchored));
            m_negated.push_back(negated);
            m_directoryOnly.push_back(directoryOnly);
        }
    }

    /** True when `relativePath` ('/' separated, no leading slash) is ignored. */
    bool ignores(std::string_view relativePath, bool isDirectory) const {
        std::size_t slash = relativePath.find('/');
        while (slash != std::string_view::npos) {
            if (lastRuleIgnores(relativePath.substr(0, slash), true)) {
                return true;
            }
            slash = relativePath.find('/', slash + 1);
        }
        return lastRuleIgnores(relativePath, isDirectory);
    }

    /**
     * Verdict of the rules for exactly this path, without looking at parent directories:
     * true ignored, false re-included by a `!` rule, nullopt no rule matched.
     */
    std::optional<bool> decide(std::string_view relativePath, bool isDirectory) const {
        std::optional<bool> verdict;
        for (std::size_t i = 0; i < m_globs.size(); ++i) {
            if (m_directoryOnly[i] && !isDirectory) {
                continue;
            }
            if (m_glob.matches(m_globs[i], relativePath)) {
                verdict = !m_negated[i];
            }
        }
        return verdict;
    }

private:
    std::string toGlob(const std::string& body, bool anchored) const {
        return anchored ? body : "**/" + body;
    }

    bool lastRuleIgnores(std::string_view path, bool isDirectory) const {
        bool ignored = false;
        for (std::size_t i = 0; i < m_globs.size(); ++i) {
            if (m_directoryOnly[i] && !isDirectory) {
                continue;
            }
            if (m_glob.matches(m_globs[i], path)) {
                ignored = !m_negated[i];
            }
        }
        return ignored;
    }

    std::vector<std::string> m_globs;
    std::vector<bool> m_negated;
    std::vector<bool> m_directoryOnly;
    GlobMatcher m_glob;
};
