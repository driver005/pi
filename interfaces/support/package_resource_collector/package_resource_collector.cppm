export module pi.support.package_resource_collector;

import std;
export import pi.platform.i_file_system;
export import pi.types.package_filter;
export import pi.types.package_resources;
import pi.support.glob_matcher;
import pi.support.package_patterns;
import pi.types.json;

/**
 * Finds what an installed package offers. The manifest is the `pi` object of its package.json (`skills`, `prompts` and `plugins`
 * arrays of files, directories and globs, with `!`, `+` and `-` entries narrowing them); a package without one offers the
 * conventional `skills/`, `prompts/` and `plugins/` directories. Skills are SKILL.md files (a directory holding one is one skill;
 * `.md` files directly in the root are skills too), prompt templates the `.md` files of a directory and plugins its `.so` and
 * `.dylib` files. A settings filter then narrows or, with `autoload: false`, adjusts that set. Port of the collection half of
 * core/package-manager.ts (without themes and JavaScript extensions).
 */
export class PackageResourceCollector {
public:
    explicit PackageResourceCollector(IFileSystem& files)
        : m_files(files) {}

    PackageResources collect(const std::string& root, const std::optional<PackageFilter>& filter) const {
        PackageResources out;
        const std::optional<Json> manifest = readManifest(root);
        for (const std::string type : {"skills", "prompts", "plugins"}) {
            std::vector<PackageResourceEntry>& target = type == "skills" ? out.skills : (type == "prompts" ? out.prompts : out.plugins);
            if (filter) {
                collectFiltered(root, type, *filter, manifest, target);
            } else if (manifest) {
                const auto entries = manifestEntries(*manifest, type);
                for (const std::string& path : entries ? enabledByManifest(root, *entries, type) : std::vector<std::string>{}) {
                    target.push_back({path, true});
                }
            } else {
                for (const std::string& path : conventionFiles(root, type)) {
                    target.push_back({path, true});
                }
            }
        }
        return out;
    }

private:
    void collectFiltered(const std::string& root, const std::string& type, const PackageFilter& filter, const std::optional<Json>& manifest, std::vector<PackageResourceEntry>& target) const {
        const std::optional<std::vector<std::string>>& patterns = patternsOf(filter, type);
        if (filter.autoload == false) {
            if (!patterns || patterns->empty()) {
                return;
            }
            for (const auto& [path, enabled] : m_patterns.delta(offered(root, type, manifest), *patterns, root)) {
                target.push_back({path, enabled});
            }
            return;
        }
        if (!patterns) {
            const auto entries = manifest ? manifestEntries(*manifest, type) : std::nullopt;
            const std::vector<std::string> paths = entries ? enabledByManifest(root, *entries, type) : (manifest ? std::vector<std::string>{} : conventionFiles(root, type));
            for (const std::string& path : paths) {
                target.push_back({path, true});
            }
            return;
        }
        const std::vector<std::string> all = offered(root, type, manifest);
        const std::vector<std::string> kept = patterns->empty() ? std::vector<std::string>{} : m_patterns.apply(all, *patterns, root);
        for (const std::string& path : all) {
            target.push_back({path, std::ranges::find(kept, path) != kept.end()});
        }
    }

    const std::optional<std::vector<std::string>>& patternsOf(const PackageFilter& filter, const std::string& type) const {
        if (type == "skills") {
            return filter.skills;
        }
        return type == "prompts" ? filter.prompts : filter.plugins;
    }

    /** Every file the package offers for a type after its own manifest patterns (what filters then work on). */
    std::vector<std::string> offered(const std::string& root, const std::string& type, const std::optional<Json>& manifest) const {
        const auto entries = manifest ? manifestEntries(*manifest, type) : std::nullopt;
        if (entries && !entries->empty()) {
            return enabledByManifest(root, *entries, type);
        }
        return conventionFiles(root, type);
    }

    std::optional<Json> readManifest(const std::string& root) const {
        const auto text = m_files.readFile(root + "/package.json");
        if (!text) {
            return std::nullopt;
        }
        const Json json = Json::parse(*text, nullptr, false);
        if (json.is_discarded() || !json.is_object() || !json.contains("pi") || !json["pi"].is_object()) {
            return std::nullopt;
        }
        return json["pi"];
    }

    std::optional<std::vector<std::string>> manifestEntries(const Json& manifest, const std::string& type) const {
        if (!manifest.contains(type) || !manifest[type].is_array()) {
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const Json& item : manifest[type]) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
        return out;
    }

    std::vector<std::string> enabledByManifest(const std::string& root, const std::vector<std::string>& entries, const std::string& type) const {
        std::vector<std::string> sources;
        std::vector<std::string> overrides;
        for (const std::string& entry : entries) {
            (m_patterns.isOverride(entry) ? overrides : sources).push_back(entry);
        }
        std::vector<std::string> paths;
        for (const std::string& entry : sources) {
            if (m_patterns.hasGlob(entry)) {
                for (std::string& match : expandGlob(entry, root)) {
                    paths.push_back(std::move(match));
                }
            } else {
                paths.push_back(std::filesystem::path(root + "/" + entry).lexically_normal().generic_string());
            }
        }
        const std::vector<std::string> all = filesFromPaths(paths, type);
        return overrides.empty() ? all : m_patterns.apply(all, overrides, root);
    }

    std::vector<std::string> filesFromPaths(const std::vector<std::string>& paths, const std::string& type) const {
        std::vector<std::string> files;
        for (const std::string& path : paths) {
            const auto stat = m_files.stat(path);
            if (!stat) {
                continue;
            }
            if (stat->isFile) {
                files.push_back(path);
            } else if (stat->isDirectory) {
                for (std::string& file : directoryFiles(path, type)) {
                    files.push_back(std::move(file));
                }
            }
        }
        return files;
    }

    std::vector<std::string> conventionFiles(const std::string& root, const std::string& type) const {
        const std::string directory = root + "/" + type;
        const auto stat = m_files.stat(directory);
        return stat && stat->isDirectory ? directoryFiles(directory, type) : std::vector<std::string>{};
    }

    std::vector<std::string> directoryFiles(const std::string& directory, const std::string& type) const {
        std::vector<std::string> out;
        if (type == "skills") {
            skillFiles(directory, directory, out);
            return out;
        }
        for (const std::string& name : sortedNames(directory)) {
            const std::string path = directory + "/" + name;
            const auto stat = m_files.stat(path);
            const bool wanted = type == "prompts" ? name.ends_with(".md") : (name.ends_with(".so") || name.ends_with(".dylib"));
            if (!name.starts_with(".") && stat && stat->isFile && wanted) {
                out.push_back(path);
            }
        }
        return out;
    }

    void skillFiles(const std::string& directory, const std::string& root, std::vector<std::string>& out) const {
        const std::vector<std::string> names = sortedNames(directory);
        const std::string marker = directory + "/SKILL.md";
        if (const auto stat = m_files.stat(marker); stat && stat->isFile) {
            out.push_back(marker);
            return;
        }
        for (const std::string& name : names) {
            if (name.starts_with(".") || name == "node_modules") {
                continue;
            }
            const std::string path = directory + "/" + name;
            const auto stat = m_files.stat(path);
            if (!stat) {
                continue;
            }
            if (stat->isFile && name.ends_with(".md") && directory == root) {
                out.push_back(path);
            } else if (stat->isDirectory) {
                skillFiles(path, root, out);
            }
        }
    }

    std::vector<std::string> sortedNames(const std::string& directory) const {
        auto names = m_files.listDirectory(directory);
        if (!names) {
            return {};
        }
        std::ranges::sort(*names);
        return *names;
    }

    /** Files and directories under `root` whose path relative to it matches the glob; hidden entries and node_modules are skipped. */
    std::vector<std::string> expandGlob(const std::string& pattern, const std::string& root) const {
        std::vector<std::string> out;
        walk(root, "", pattern, out);
        std::ranges::sort(out);
        return out;
    }

    void walk(const std::string& directory, const std::string& relative, const std::string& pattern, std::vector<std::string>& out) const {
        for (const std::string& name : sortedNames(directory)) {
            if (name.starts_with(".") || name == "node_modules") {
                continue;
            }
            const std::string path = directory + "/" + name;
            const std::string rel = relative.empty() ? name : relative + "/" + name;
            if (m_glob.matches(pattern, rel)) {
                out.push_back(path);
            }
            if (const auto stat = m_files.stat(path); stat && stat->isDirectory) {
                walk(path, rel, pattern, out);
            }
        }
    }

    IFileSystem& m_files;
    GlobMatcher m_glob;
    PackagePatterns m_patterns;
};
