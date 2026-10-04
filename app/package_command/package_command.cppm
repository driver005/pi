export module pi.package_command;

import std;
export import pi.coding_services;
export import pi.types.command_line;
import pi.session.settings_manager;
import pi.support.package_manager;

/**
 * `pi install <source> [-l]`, `pi remove <source> [-l]`, `pi update [source]` and `pi list`: packages of skills, prompt templates
 * and plugins (PackageManager). Sources are `git:github.com/user/repo[@ref]`, `https://...` and `ssh://...` URLs and local
 * directories; `-l` uses the project's settings (only for a trusted project) instead of the global ones. Port of the package
 * commands of cli.ts without npm sources. Returns the process exit code.
 */
export class PackageCommand {
public:
    PackageCommand(CodingServices& services, std::ostream& out, std::ostream& err)
        : m_services(services),
          m_out(out),
          m_err(err) {}

    int run(const CommandLine& line) {
        const std::string& agentDir = line.options.agentDir;
        PlatformServices& platform = m_services.platform();
        SettingsManager settings(agentDir + "/settings.json", line.options.cwd + "/.pi/settings.json", false, platform.files(), platform.locks(), platform.ids());
        const auto trusted = m_services.trust().resolve(line.options.cwd, line.options.startup.trustProject, settings.view().defaultProjectTrust(), {});
        if (trusted && *trusted) {
            settings.setProjectTrusted(true);
        }
        PackageManager packages(platform.files(), platform.processes(), settings, line.options.cwd, agentDir);
        if (line.command == "list") {
            return list(packages);
        }
        if (line.command == "update") {
            return update(packages, line.arguments.empty() ? std::nullopt : std::optional<std::string>(line.arguments[0]));
        }
        if (line.command == "remove") {
            return remove(packages, line.arguments[0], line.localPackages);
        }
        return install(packages, line.arguments[0], line.localPackages);
    }

private:
    int install(PackageManager& packages, const std::string& source, bool project) {
        if (const auto installed = packages.install(source, project); !installed) {
            return fail(installed.error().message);
        }
        m_out << "Installed " << source << "\n";
        return 0;
    }

    int remove(PackageManager& packages, const std::string& source, bool project) {
        const auto removed = packages.remove(source, project);
        if (!removed) {
            return fail(removed.error().message);
        }
        if (!*removed) {
            return fail("No matching package found for " + source);
        }
        m_out << "Removed " << source << "\n";
        return 0;
    }

    int update(PackageManager& packages, const std::optional<std::string>& source) {
        const auto updated = packages.update(source);
        if (!updated) {
            return fail(updated.error().message);
        }
        for (const std::string& entry : *updated) {
            m_out << "Updated " << entry << "\n";
        }
        if (updated->empty()) {
            m_out << "No packages to update.\n";
        }
        return 0;
    }

    int list(PackageManager& packages) {
        const std::vector<ConfiguredPackage> configured = packages.list();
        if (configured.empty()) {
            m_out << "No packages installed.\n";
        }
        for (const std::string scope : {"user", "project"}) {
            bool header = false;
            for (const ConfiguredPackage& entry : configured) {
                if (entry.scope != scope) {
                    continue;
                }
                if (!header) {
                    m_out << (scope == "user" ? "User packages:\n" : "Project packages:\n");
                    header = true;
                }
                m_out << "  " << entry.source << (entry.filtered ? " (filtered)" : "") << "\n";
                m_out << "    " << (entry.installedPath ? *entry.installedPath : "not installed") << "\n";
            }
        }
        return 0;
    }

    int fail(const std::string& message) {
        m_err << "pi: " << message << "\n";
        return 1;
    }

    CodingServices& m_services;
    std::ostream& m_out;
    std::ostream& m_err;
};
