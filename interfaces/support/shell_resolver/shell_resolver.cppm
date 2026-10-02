export module pi.support.shell_resolver;

import std;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;

/** Picks the shell that runs commands: a configured path, /bin/bash, bash from PATH, else sh. */
export class ShellResolver {
public:
    ShellResolver(IFileSystem& files, IEnvironment& environment);

    std::string resolve(const std::string& configuredPath) const;

private:
    std::optional<std::string> searchPath() const;

    IFileSystem& m_files;
    IEnvironment& m_environment;
};

ShellResolver::ShellResolver(IFileSystem& files, IEnvironment& environment)
    : m_files(files), m_environment(environment) {}

std::optional<std::string> ShellResolver::searchPath() const {
    const auto path = m_environment.get("PATH");
    if (!path) {
        return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= path->size()) {
        std::size_t end = path->find(':', start);
        end = end == std::string::npos ? path->size() : end;
        const std::string candidate = path->substr(start, end - start) + "/bash";
        if (m_files.exists(candidate)) {
            return candidate;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::string ShellResolver::resolve(const std::string& configuredPath) const {
    if (!configuredPath.empty() && m_files.exists(configuredPath)) {
        return configuredPath;
    }
    if (m_files.exists("/bin/bash")) {
        return "/bin/bash";
    }
    return searchPath().value_or("sh");
}
