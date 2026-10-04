export module pi.support.package_entries;

import std;
export import pi.types.configured_package;
export import pi.types.json;
export import pi.support.package_source_parser;

/**
 * The `packages` array of a settings file. An entry is a source string or an object `{source, autoload?, skills?, prompts?, plugins?}`.
 * Entries are matched by package identity, so `git:github.com/a/b@v2` finds the entry written as `git:github.com/a/b`.
 */
export class PackageEntries {
public:
    explicit PackageEntries(const PackageSourceParser& parser)
        : m_parser(parser) {}

    std::vector<ConfiguredPackage> read(const Json& packages, const std::string& scope) const {
        std::vector<ConfiguredPackage> out;
        if (!packages.is_array()) {
            return out;
        }
        for (const Json& entry : packages) {
            const std::string source = sourceOf(entry);
            if (source.empty()) {
                continue;
            }
            ConfiguredPackage configured;
            configured.source = source;
            configured.scope = scope;
            configured.filtered = entry.is_object();
            if (entry.is_object()) {
                configured.filter = filterOf(entry);
            }
            out.push_back(std::move(configured));
        }
        return out;
    }

    std::string sourceOf(const Json& entry) const {
        if (entry.is_string()) {
            return entry.get<std::string>();
        }
        if (entry.is_object() && entry.contains("source") && entry["source"].is_string()) {
            return entry["source"].get<std::string>();
        }
        return "";
    }

    /** The array with `source` added (a local path in its settings form), or nullopt when it already is there as written. */
    std::optional<Json> added(const Json& packages, const std::string& source, const std::string& baseDir) const {
        Json next = packages.is_array() ? packages : Json::array();
        const std::string stored = m_parser.settingsForm(source, baseDir);
        const std::size_t at = find(next, source, baseDir);
        if (at == next.size()) {
            next.push_back(stored);
            return next;
        }
        if (sourceOf(next[at]) == stored) {
            return std::nullopt;
        }
        if (next[at].is_object()) {
            next[at]["source"] = stored;
        } else {
            next[at] = stored;
        }
        return next;
    }

    /** The array without the entries naming the same package as `source`, or nullopt when none does. */
    std::optional<Json> removed(const Json& packages, const std::string& source, const std::string& baseDir) const {
        if (!packages.is_array()) {
            return std::nullopt;
        }
        const std::string identity = m_parser.identity(source, baseDir);
        Json next = Json::array();
        for (const Json& entry : packages) {
            if (m_parser.identity(sourceOf(entry), baseDir) != identity) {
                next.push_back(entry);
            }
        }
        if (next.size() == packages.size()) {
            return std::nullopt;
        }
        return next;
    }

private:
    std::size_t find(const Json& packages, const std::string& source, const std::string& baseDir) const {
        const std::string identity = m_parser.identity(source, baseDir);
        for (std::size_t i = 0; i < packages.size(); ++i) {
            const std::string existing = sourceOf(packages[i]);
            if (!existing.empty() && m_parser.identity(existing, baseDir) == identity) {
                return i;
            }
        }
        return packages.size();
    }

    PackageFilter filterOf(const Json& entry) const {
        PackageFilter filter;
        if (entry.contains("autoload") && entry["autoload"].is_boolean()) {
            filter.autoload = entry["autoload"].get<bool>();
        }
        filter.skills = list(entry, "skills");
        filter.prompts = list(entry, "prompts");
        filter.plugins = list(entry, "plugins");
        return filter;
    }

    std::optional<std::vector<std::string>> list(const Json& entry, const std::string& key) const {
        if (!entry.contains(key) || !entry[key].is_array()) {
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const Json& item : entry[key]) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
        return out;
    }

    const PackageSourceParser& m_parser;
};
