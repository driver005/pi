export module pi.support.layered_ignore;

import std;
export import pi.support.gitignore_matcher;

/**
 * Ignore rules collected while descending a tree: each directory's .gitignore/.ignore/.fdignore
 * is added with its position, deeper rules come later and win. Mirrors feeding prefixed patterns
 * into one `ignore` instance in the TypeScript skill loader.
 */
export class LayeredIgnore {
public:
    /** Rules of the ignore file at `directory` ('/' separated, relative to the root, "" = root). */
    void addLayer(const std::string& directory, std::string_view rulesText);

    /** True when the path (relative to the root) or one of its parent directories is ignored. */
    bool ignores(const std::string& relativePath, bool isDirectory) const;

    bool empty() const;

private:
    std::optional<bool> verdict(const std::string& path, bool isDirectory) const;

    std::vector<std::pair<std::string, GitignoreMatcher>> m_layers;
};

void LayeredIgnore::addLayer(const std::string& directory, std::string_view rulesText) {
    m_layers.emplace_back(directory, GitignoreMatcher(rulesText));
}

bool LayeredIgnore::empty() const {
    return m_layers.empty();
}

std::optional<bool> LayeredIgnore::verdict(const std::string& path, bool isDirectory) const {
    std::optional<bool> result;
    for (const auto& [directory, matcher] : m_layers) {
        std::string relative;
        if (directory.empty()) {
            relative = path;
        } else if (path.size() > directory.size() && path.compare(0, directory.size(), directory) == 0 &&
                   path[directory.size()] == '/') {
            relative = path.substr(directory.size() + 1);
        } else {
            continue;
        }
        if (const auto decision = matcher.decide(relative, isDirectory)) {
            result = decision;
        }
    }
    return result;
}

bool LayeredIgnore::ignores(const std::string& relativePath, bool isDirectory) const {
    std::size_t slash = relativePath.find('/');
    while (slash != std::string::npos) {
        if (verdict(relativePath.substr(0, slash), true) == true) {
            return true;
        }
        slash = relativePath.find('/', slash + 1);
    }
    return verdict(relativePath, isDirectory) == true;
}
