export module pi.support.npm_command;

import std;
export import pi.platform.i_file_system;
export import pi.platform.i_process_runner;
export import pi.session.i_settings_manager;
export import pi.types.result;
import pi.support.semver_comparator;

/**
 * Runs the package manager for npm packages: installs and removes them in a managed directory (`<install root>/node_modules/<name>`,
 * with a private `package.json` and a `.gitignore` made on first use), asks the registry for the newest version of a specification
 * and reads the installed version. The command is the `npmCommand` setting (default `npm`); npm, pnpm and bun, also when wrapped
 * by another command, get their own arguments, with peer dependencies off like the TypeScript managed installs (packages that run
 * inside pi must not pull in pi itself). Port of the npm half of DefaultPackageManager in core/package-manager.ts.
 */
export class NpmCommand {
public:
    NpmCommand(IFileSystem& files, IProcessRunner& processes, const ISettingsManager& settings)
        : m_files(files),
          m_processes(processes),
          m_settings(settings) {}

    /** Installs the specifications (`name`, `name@1.2.3`, `name@^1`) under `root`, creating the managed project first. */
    Result<void> install(const std::vector<std::string>& specs, const std::string& root) {
        if (auto ready = ensureProject(root); !ready) {
            return ready;
        }
        const std::string manager = packageManagerName();
        std::vector<std::string> args{"install"};
        args.insert(args.end(), specs.begin(), specs.end());
        if (manager == "bun") {
            args.insert(args.end(), {"--cwd", root, "--omit=peer"});
        } else if (manager == "pnpm") {
            args.insert(args.end(), {"--prefix", root, "--config.auto-install-peers=false", "--config.strict-peer-dependencies=false", "--config.strict-dep-builds=false"});
        } else {
            args.insert(args.end(), {"--prefix", root, "--legacy-peer-deps"});
        }
        return run(args, std::nullopt, std::chrono::minutes(10)).transform([](const std::string&) {});
    }

    /** Removes the package from the managed project; nothing to do when there is none. */
    Result<void> uninstall(const std::string& name, const std::string& root) {
        if (!m_files.exists(root)) {
            return {};
        }
        const std::string manager = packageManagerName();
        std::vector<std::string> args{"uninstall", name};
        if (manager == "bun") {
            args.insert(args.end(), {"--cwd", root});
        } else {
            args.insert(args.end(), {"--prefix", root});
            if (manager != "pnpm") {
                args.push_back("--legacy-peer-deps");
            }
        }
        return run(args, std::nullopt, std::chrono::minutes(10)).transform([](const std::string&) {});
    }

    /** The newest version `npm view <spec> version --json` reports (for a range, the highest of the matches). */
    Result<std::string> latestVersion(const std::string& spec, const std::string& cwd) {
        auto output = run({"view", spec, "version", "--json"}, cwd, std::chrono::seconds(30));
        if (!output) {
            return std::unexpected(output.error());
        }
        const Json parsed = Json::parse(*output, nullptr, false);
        if (parsed.is_string()) {
            return parsed.get<std::string>();
        }
        if (parsed.is_array()) {
            std::optional<std::string> best;
            for (const Json& entry : parsed) {
                if (entry.is_string() && (!best || m_semver.compare(entry.get<std::string>(), *best).value_or(-1) > 0)) {
                    best = entry.get<std::string>();
                }
            }
            if (best) {
                return *best;
            }
        }
        return std::unexpected(Error{"npm_failed", "Unexpected response from npm view"});
    }

    /** The `version` of the package.json in `packagePath`, nullopt when there is none. */
    std::optional<std::string> installedVersion(const std::string& packagePath) {
        const auto content = m_files.readFile(packagePath + "/package.json");
        if (!content) {
            return std::nullopt;
        }
        const Json manifest = Json::parse(content->starts_with("\xEF\xBB\xBF") ? content->substr(3) : *content, nullptr, false);
        if (manifest.is_object() && manifest.contains("version") && manifest["version"].is_string()) {
            return manifest["version"].get<std::string>();
        }
        return std::nullopt;
    }

    /** The package manager's own name: npm, pnpm or bun (what the command or the command it wraps is). */
    std::string packageManagerName() const {
        const std::vector<std::string> configured = m_settings.view().npmCommand();
        if (configured.empty()) {
            return "npm";
        }
        const std::string direct = commandName(configured.front());
        const std::vector<std::string> args(configured.begin() + 1, configured.end());
        const auto separator = std::ranges::find(args.rbegin(), args.rend(), "--");
        if (separator != args.rend()) {
            const auto wrapped = separator.base();
            return wrapped == args.end() ? direct : commandName(*wrapped);
        }
        if (known(direct)) {
            return direct;
        }
        std::set<std::string> found;
        for (const std::string& arg : args) {
            if (known(commandName(arg))) {
                found.insert(commandName(arg));
            }
        }
        return found.size() == 1 ? *found.begin() : direct;
    }

private:
    bool known(const std::string& name) const {
        return name == "npm" || name == "pnpm" || name == "bun";
    }

    /** The file name of a command without `.cmd` or `.exe`. */
    std::string commandName(const std::string& command) const {
        std::string name = std::filesystem::path(command).filename().string();
        for (const char* suffix : {".cmd", ".exe"}) {
            if (name.size() > std::string_view(suffix).size() && name.ends_with(suffix)) {
                name.erase(name.size() - std::string_view(suffix).size());
            }
        }
        return name;
    }

    /** `<root>` with a package.json and a .gitignore, as the managed project of npm packages. */
    Result<void> ensureProject(const std::string& root) {
        if (auto made = m_files.createDirectories(root); !made) {
            return made;
        }
        if (!m_files.exists(root + "/.gitignore")) {
            if (auto written = m_files.writeFile(root + "/.gitignore", "*\n!.gitignore\n"); !written) {
                return written;
            }
        }
        if (!m_files.exists(root + "/package.json")) {
            return m_files.writeFile(root + "/package.json", Json{{"name", "pi-extensions"}, {"private", true}}.dump(2));
        }
        return {};
    }

    Result<std::string> run(const std::vector<std::string>& args, const std::optional<std::string>& cwd, std::chrono::milliseconds timeout) {
        const std::vector<std::string> configured = m_settings.view().npmCommand();
        ProcessRequest request;
        request.command = configured.empty() ? "npm" : configured.front();
        if (request.command.empty()) {
            return std::unexpected(Error{"npm_failed", "Invalid npmCommand: the first entry must be a command"});
        }
        if (!configured.empty()) {
            request.args.assign(configured.begin() + 1, configured.end());
        }
        request.args.insert(request.args.end(), args.begin(), args.end());
        request.cwd = cwd;
        request.timeout = timeout;
        const auto result = m_processes.run(request);
        if (!result) {
            return std::unexpected(Error{"npm_failed", request.command + " " + args.front() + " could not run: " + result.error().message});
        }
        if (result->exitCode != 0 || result->timedOut) {
            return std::unexpected(Error{"npm_failed", request.command + " " + args.front() + (result->timedOut ? " timed out" : " failed: " + tail(result->output))});
        }
        return result->output;
    }

    std::string tail(const std::string& output) const {
        const std::size_t first = output.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        const std::string trimmed = output.substr(first, output.find_last_not_of(" \t\r\n") - first + 1);
        return trimmed.size() > 2000 ? "..." + trimmed.substr(trimmed.size() - 2000) : trimmed;
    }

    IFileSystem& m_files;
    IProcessRunner& m_processes;
    const ISettingsManager& m_settings;
    SemverComparator m_semver;
};
