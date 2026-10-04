export module pi.support.package_manager;

import std;
export import pi.platform.i_file_system;
export import pi.platform.i_process_runner;
export import pi.session.i_settings_manager;
export import pi.types.configured_package;
export import pi.types.package_paths;
export import pi.types.package_resolution;
export import pi.types.result;
import pi.support.npm_command;
import pi.support.package_entries;
import pi.support.package_resource_collector;
import pi.support.package_source_parser;
import pi.support.semver_comparator;

/**
 * Installs, removes, updates and resolves the packages named by the `packages` list of the global and project settings. A package
 * is a directory (or git repository) offering skills, prompt templates and plugins (see PackageResourceCollector). Git packages
 * are cloned to `<agent dir>/git/<host>/<path>` (project scope: `<cwd>/.pi/git/...`, only for a trusted project), local packages
 * are used where they are (a relative path counts from the agent dir, or from `<cwd>/.pi` for the project), npm packages are
 * installed by the configured package manager (NpmCommand) under `<agent dir>/npm` (project scope: `<cwd>/.pi/npm`); their skills,
 * prompt templates and plugins are used, JavaScript extensions are not. A package named in both scopes is used from the project; a project entry with
 * `autoload: false` instead adjusts the user package's resources. Port of the git and settings half of DefaultPackageManager in
 * core/package-manager.ts.
 */
export class PackageManager {
public:
    PackageManager(IFileSystem& files, IProcessRunner& processes, ISettingsManager& settings, std::string cwd, std::string agentDir)
        : m_files(files),
          m_processes(processes),
          m_settings(settings),
          m_cwd(std::move(cwd)),
          m_agentDir(std::move(agentDir)),
          m_parser(files.homeDirectory()),
          m_entries(m_parser),
          m_collector(files),
          m_npm(files, processes, settings) {}

    std::vector<ConfiguredPackage> list() const {
        std::vector<ConfiguredPackage> out = m_entries.read(packagesOf("user"), "user");
        for (ConfiguredPackage& entry : m_entries.read(packagesOf("project"), "project")) {
            out.push_back(std::move(entry));
        }
        for (ConfiguredPackage& entry : out) {
            entry.installedPath = installedPath(entry.source, entry.scope);
        }
        return out;
    }

    /** Installs the package (git: clone) and records it in the settings of the scope. */
    Result<void> install(const std::string& source, bool project) {
        const std::string scope = project ? "project" : "user";
        if (auto access = checkScope(scope); !access) {
            return access;
        }
        const PackageSource parsed = m_parser.parse(source);
        std::string recorded = source;
        if (parsed.type == "npm") {
            if (auto installed = m_npm.install({parsed.npmSpec}, npmRoot(scope)); !installed) {
                return installed;
            }
        } else if (parsed.type == "git") {
            if (auto installed = installGit(parsed, scope); !installed) {
                return installed;
            }
        } else {
            recorded = m_parser.resolveLocal(parsed.localPath, m_cwd);
            if (!m_files.exists(recorded)) {
                return std::unexpected(Error{"missing_path", "Path does not exist: " + recorded});
            }
        }
        return record(scope, m_entries.added(packagesOf(scope), recorded, baseDir(scope)));
    }

    /** Deletes a git checkout or uninstalls an npm package and drops the entry; true when the settings named the package. */
    Result<bool> remove(const std::string& source, bool project) {
        const std::string scope = project ? "project" : "user";
        if (auto access = checkScope(scope); !access) {
            return std::unexpected(access.error());
        }
        const PackageSource parsed = m_parser.parse(source);
        std::string recorded = source;
        if (parsed.type == "npm") {
            if (auto removed = m_npm.uninstall(parsed.npmName, npmRoot(scope)); !removed) {
                return std::unexpected(removed.error());
            }
        } else if (parsed.type == "git") {
            if (auto removed = removeGit(parsed, scope); !removed) {
                return std::unexpected(removed.error());
            }
        } else {
            recorded = m_parser.resolveLocal(parsed.localPath, m_cwd);
        }
        const std::optional<Json> next = m_entries.removed(packagesOf(scope), recorded, baseDir(scope));
        if (auto written = record(scope, next); !written) {
            return std::unexpected(written.error());
        }
        return next.has_value();
    }

    /**
     * Brings the git packages of both scopes (or the one matching `source`) to the newest commit of their upstream and the npm
     * packages to their newest version (the one their specification allows); pinned packages stay. Returns the sources it looked at. A failing package does not stop the others; the first failure is returned
     * once all have run.
     */
    Result<std::vector<std::string>> update(const std::optional<std::string>& source) {
        std::vector<std::string> updated;
        std::optional<Error> failure;
        bool matched = false;
        for (const ConfiguredPackage& entry : configured()) {
            if (source && m_parser.identity(*source, baseDir(entry.scope)) != m_parser.identity(entry.source, baseDir(entry.scope))) {
                continue;
            }
            matched = true;
            const PackageSource parsed = m_parser.parse(entry.source);
            if (parsed.type == "local" || parsed.pinned) {
                continue;
            }
            const Result<void> done = parsed.type == "npm" ? updateNpm(parsed, entry.scope) : updateGit(parsed, entry.scope);
            if (done) {
                updated.push_back(entry.source);
            } else if (!failure) {
                failure = done.error();
            }
        }
        if (source && !matched) {
            return std::unexpected(Error{"no_such_package", "No matching package found for " + *source});
        }
        if (failure) {
            return std::unexpected(*failure);
        }
        return updated;
    }

    /** What the configured packages offer; git packages that are missing are cloned first when `installMissing`. */
    PackageResolution resolve(bool installMissing) {
        PackageResolution out;
        const std::vector<ConfiguredPackage> entries = deduped(configured());
        for (const ConfiguredPackage& entry : entries) {
            const ConfiguredPackage& base = deltaBase(entry, entries);
            const std::optional<std::string> root = packageRoot(base, installMissing, out.warnings);
            if (!root) {
                continue;
            }
            const PackageResources found = m_collector.collect(*root, entry.filtered ? std::optional<PackageFilter>(entry.filter) : std::nullopt);
            merge(out.resources.skills, found.skills);
            merge(out.resources.prompts, found.prompts);
            merge(out.resources.plugins, found.plugins);
        }
        return out;
    }

    /** The enabled files of every package, for the loaders of skills, prompt templates and plugins. */
    PackagePaths load(bool installMissing) {
        const PackageResolution resolution = resolve(installMissing);
        PackagePaths out;
        out.skills = enabled(resolution.resources.skills);
        out.prompts = enabled(resolution.resources.prompts);
        out.plugins = enabled(resolution.resources.plugins);
        out.warnings = resolution.warnings;
        return out;
    }

    std::vector<std::string> enabled(const std::vector<PackageResourceEntry>& resources) const {
        std::vector<std::string> out;
        for (const PackageResourceEntry& entry : resources) {
            if (entry.enabled) {
                out.push_back(entry.path);
            }
        }
        return out;
    }

private:
    /** Project entries first (their resources win a collision), then user entries. */
    std::vector<ConfiguredPackage> configured() const {
        std::vector<ConfiguredPackage> out;
        if (m_settings.projectTrusted()) {
            out = m_entries.read(packagesOf("project"), "project");
        }
        for (ConfiguredPackage& entry : m_entries.read(packagesOf("user"), "user")) {
            out.push_back(std::move(entry));
        }
        return out;
    }

    std::vector<ConfiguredPackage> deduped(const std::vector<ConfiguredPackage>& entries) const {
        std::vector<ConfiguredPackage> out;
        std::map<std::string, std::size_t> seen;
        for (const ConfiguredPackage& entry : entries) {
            const std::string identity = m_parser.identity(entry.source, baseDir(entry.scope));
            const auto found = seen.find(identity);
            if (found == seen.end()) {
                seen[identity] = out.size();
                out.push_back(entry);
                continue;
            }
            const ConfiguredPackage& existing = out[found->second];
            if (existing.scope == "project" && entry.scope == "user") {
                if (existing.filtered && existing.filter.autoload == false) {
                    out.push_back(entry);
                }
            } else if (entry.scope == "project") {
                out[found->second] = entry;
            }
        }
        return out;
    }

    /** A project entry with `autoload: false` works on the user package of the same identity; every other entry on itself. */
    const ConfiguredPackage& deltaBase(const ConfiguredPackage& entry, const std::vector<ConfiguredPackage>& entries) const {
        if (entry.scope != "project" || !entry.filtered || entry.filter.autoload != false) {
            return entry;
        }
        const std::string identity = m_parser.identity(entry.source, baseDir("project"));
        for (const ConfiguredPackage& candidate : entries) {
            if (candidate.scope == "user" && m_parser.identity(candidate.source, baseDir("user")) == identity) {
                return candidate;
            }
        }
        return entry;
    }

    std::optional<std::string> packageRoot(const ConfiguredPackage& entry, bool installMissing, std::vector<std::string>& warnings) {
        const PackageSource parsed = m_parser.parse(entry.source);
        if (parsed.type == "npm") {
            return npmPackageRoot(parsed, entry, installMissing, warnings);
        }
        if (parsed.type == "local") {
            const std::string path = m_parser.resolveLocal(parsed.localPath, baseDir(entry.scope));
            return m_files.exists(path) ? std::optional<std::string>(path) : std::nullopt;
        }
        const std::string path = gitPath(parsed, entry.scope);
        if (m_files.exists(path)) {
            return path;
        }
        if (!installMissing) {
            return std::nullopt;
        }
        if (auto installed = installGit(parsed, entry.scope); !installed) {
            warnings.push_back("Package " + entry.source + ": " + installed.error().message);
            return std::nullopt;
        }
        return path;
    }

    void merge(std::vector<PackageResourceEntry>& into, const std::vector<PackageResourceEntry>& from) const {
        for (const PackageResourceEntry& entry : from) {
            if (std::ranges::none_of(into, [&entry](const PackageResourceEntry& have) { return have.path == entry.path; })) {
                into.push_back(entry);
            }
        }
    }

    std::optional<std::string> installedPath(const std::string& source, const std::string& scope) const {
        const PackageSource parsed = m_parser.parse(source);
        std::string path;
        if (parsed.type == "git") {
            path = gitPath(parsed, scope);
        } else if (parsed.type == "npm") {
            path = npmPath(parsed, scope);
        } else {
            path = m_parser.resolveLocal(parsed.localPath, baseDir(scope));
        }
        return m_files.exists(path) ? std::optional<std::string>(path) : std::nullopt;
    }

    /** The installed package; installed first when it is missing, or older than the exact version the entry pins, and `installMissing`. */
    std::optional<std::string> npmPackageRoot(const PackageSource& source, const ConfiguredPackage& entry, bool installMissing, std::vector<std::string>& warnings) {
        const std::string path = npmPath(source, entry.scope);
        const std::optional<std::string> version = m_files.exists(path) ? m_npm.installedVersion(path) : std::nullopt;
        const bool matches = version && (!source.pinned || version == source.npmVersion);
        if (matches) {
            return path;
        }
        if (!installMissing) {
            return m_files.exists(path) ? std::optional<std::string>(path) : std::nullopt;
        }
        if (auto installed = m_npm.install({source.npmSpec}, npmRoot(entry.scope)); !installed) {
            warnings.push_back("Package " + entry.source + ": " + installed.error().message);
            return std::nullopt;
        }
        return path;
    }

    /** Installs the newest version the specification allows when it is newer than the installed one (or when that cannot be told). */
    Result<void> updateNpm(const PackageSource& source, const std::string& scope) {
        const std::optional<std::string> installed = m_npm.installedVersion(npmPath(source, scope));
        const std::string spec = source.npmVersion ? source.npmSpec : source.npmName + "@latest";
        if (installed) {
            const auto latest = m_npm.latestVersion(source.npmVersion ? source.npmSpec : source.npmName, m_cwd);
            const auto order = latest ? m_semver.compare(*latest, *installed) : std::nullopt;
            if (order && *order <= 0) {
                return {};
            }
        }
        return m_npm.install({spec}, npmRoot(scope));
    }

    Result<void> installGit(const PackageSource& source, const std::string& scope) {
        const std::string target = gitPath(source, scope);
        if (m_files.exists(target)) {
            return updateGit(source, scope);
        }
        const std::string root = gitRoot(scope);
        if (auto ignore = ensureIgnoreFile(root); !ignore) {
            return ignore;
        }
        if (auto made = m_files.createDirectories(parentOf(target)); !made) {
            return made;
        }
        auto cloned = git({"clone", source.repo, target}, std::nullopt);
        if (cloned && source.ref) {
            cloned = git({"checkout", *source.ref}, target);
        }
        if (!cloned) {
            m_files.removeTree(target);
            pruneEmptyParents(target, root);
            return std::unexpected(cloned.error());
        }
        return {};
    }

    Result<void> updateGit(const PackageSource& source, const std::string& scope) {
        const std::string target = gitPath(source, scope);
        if (!m_files.exists(target)) {
            return installGit(source, scope);
        }
        if (source.ref) {
            return moveTo(target, {"fetch", "origin", *source.ref}, "FETCH_HEAD");
        }
        return moveToUpstream(target);
    }

    /** Fetches the branch the checkout follows (its upstream, else the remote's default branch) and moves to its tip. */
    Result<void> moveToUpstream(const std::string& target) {
        std::string branch;
        std::string ref = "@{upstream}";
        const auto upstream = git({"rev-parse", "--abbrev-ref", "@{upstream}"}, target);
        if (upstream && trim(*upstream).starts_with("origin/") && trim(*upstream).size() > 7) {
            branch = trim(*upstream).substr(7);
        } else {
            git({"remote", "set-head", "origin", "-a"}, target);
            const auto head = git({"symbolic-ref", "refs/remotes/origin/HEAD"}, target);
            const std::string prefix = "refs/remotes/origin/";
            if (!head || !trim(*head).starts_with(prefix)) {
                return std::unexpected(Error{"git_failed", "Cannot tell which branch to follow in " + target});
            }
            branch = trim(*head).substr(prefix.size());
            ref = "origin/HEAD";
        }
        return moveTo(target, {"fetch", "--prune", "--no-tags", "origin", "+refs/heads/" + branch + ":refs/remotes/origin/" + branch}, ref);
    }

    Result<void> moveTo(const std::string& target, const std::vector<std::string>& fetchArgs, const std::string& ref) {
        if (auto fetched = git(fetchArgs, target); !fetched) {
            return std::unexpected(fetched.error());
        }
        const auto local = git({"rev-parse", "HEAD"}, target);
        const auto wanted = git({"rev-parse", ref + "^{commit}"}, target);
        if (!local) {
            return std::unexpected(local.error());
        }
        if (!wanted) {
            return std::unexpected(wanted.error());
        }
        if (trim(*local) == trim(*wanted)) {
            return {};
        }
        if (auto reset = git({"reset", "--hard", trim(*wanted)}, target); !reset) {
            return std::unexpected(reset.error());
        }
        // The checkout is the package, nothing else: drop what the old version left behind.
        if (auto clean = git({"clean", "-fdx"}, target); !clean) {
            return std::unexpected(clean.error());
        }
        return {};
    }

    Result<void> removeGit(const PackageSource& source, const std::string& scope) {
        const std::string target = gitPath(source, scope);
        if (auto removed = m_files.removeTree(target); !removed) {
            return removed;
        }
        pruneEmptyParents(target, gitRoot(scope));
        return {};
    }

    void pruneEmptyParents(const std::string& target, const std::string& root) {
        std::string current = parentOf(target);
        while (current.size() > root.size() && current.starts_with(root + "/")) {
            if (m_files.exists(current)) {
                const auto names = m_files.listDirectory(current);
                if (!names || !names->empty() || !m_files.removeTree(current)) {
                    return;
                }
            }
            current = parentOf(current);
        }
    }

    Result<void> ensureIgnoreFile(const std::string& root) {
        if (auto made = m_files.createDirectories(root); !made) {
            return made;
        }
        const std::string path = root + "/.gitignore";
        if (m_files.exists(path)) {
            return {};
        }
        return m_files.writeFile(path, "*\n!.gitignore\n");
    }

    Result<std::string> git(const std::vector<std::string>& args, const std::optional<std::string>& directory) {
        ProcessRequest request;
        request.command = "git";
        request.args = args;
        request.cwd = directory;
        request.env["GIT_TERMINAL_PROMPT"] = "0";
        request.timeout = std::chrono::minutes(5);
        const auto result = m_processes.run(request);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (result->exitCode != 0 || result->timedOut) {
            const std::string reason = result->timedOut ? "timed out" : "failed: " + trim(result->output);
            return std::unexpected(Error{"git_failed", "git " + args.front() + " " + reason});
        }
        return result->output;
    }

    Result<void> checkScope(const std::string& scope) const {
        if (scope == "project" && !m_settings.projectTrusted()) {
            return std::unexpected(Error{"untrusted_project", "Project is not trusted; refusing to access project package storage"});
        }
        return {};
    }

    /** Writes the new `packages` list when there is one. */
    Result<void> record(const std::string& scope, const std::optional<Json>& next) {
        if (!next) {
            return {};
        }
        return scope == "project" ? m_settings.setProject("packages", *next) : m_settings.setGlobal("packages", *next);
    }

    Json packagesOf(const std::string& scope) const {
        const Json settings = scope == "project" ? m_settings.projectSettings() : m_settings.globalSettings();
        return settings.is_object() && settings.contains("packages") && settings["packages"].is_array() ? settings["packages"] : Json::array();
    }

    std::string baseDir(const std::string& scope) const {
        return scope == "project" ? m_cwd + "/.pi" : m_agentDir;
    }

    std::string npmRoot(const std::string& scope) const {
        return baseDir(scope) + "/npm";
    }

    std::string npmPath(const PackageSource& source, const std::string& scope) const {
        return npmRoot(scope) + "/node_modules/" + source.npmName;
    }

    std::string gitRoot(const std::string& scope) const {
        return baseDir(scope) + "/git";
    }

    std::string gitPath(const PackageSource& source, const std::string& scope) const {
        return gitRoot(scope) + "/" + source.host + "/" + source.path;
    }

    std::string parentOf(const std::string& path) const {
        return std::filesystem::path(path).parent_path().generic_string();
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    IFileSystem& m_files;
    IProcessRunner& m_processes;
    ISettingsManager& m_settings;
    std::string m_cwd;
    std::string m_agentDir;
    PackageSourceParser m_parser;
    PackageEntries m_entries;
    PackageResourceCollector m_collector;
    NpmCommand m_npm;
    SemverComparator m_semver;
};
