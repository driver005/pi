export module pi.support.package_source_parser;

import std;
export import pi.types.package_source;
import pi.support.git_source_parser;
import pi.support.path_resolver;

/**
 * Tells what a package source string names: `npm:name@version`, a git source (see GitSourceParser; `github:user/repo` is accepted
 * as `git:github.com/user/repo`) or else a local path. Identities decide whether two entries name the same package: `npm:<name>`,
 * `git:<host>/<path>` (any ref) and `local:<absolute path>`.
 */
export class PackageSourceParser {
public:
    explicit PackageSourceParser(std::string homeDirectory)
        : m_paths(std::move(homeDirectory)) {}

    PackageSource parse(const std::string& source) const {
        const std::string trimmed = trim(source);
        PackageSource out;
        if (trimmed.starts_with("npm:")) {
            out.type = "npm";
            out.npmSpec = trim(trimmed.substr(4));
            return out;
        }
        const std::string git = trimmed.starts_with("github:") ? "git:github.com/" + trimmed.substr(7) : trimmed;
        if (const auto parsed = m_git.parse(git)) {
            return *parsed;
        }
        out.type = "local";
        out.localPath = trimmed;
        return out;
    }

    /** The package name of an npm specification (`@scope/name@1.2` is `@scope/name`). */
    std::string npmName(const std::string& spec) const {
        const std::size_t at = spec.find('@', spec.starts_with("@") ? 1 : 0);
        return at == std::string::npos ? spec : spec.substr(0, at);
    }

    /** `baseDir` is where a relative local path is taken from: the directory of the settings file's scope. */
    std::string identity(const std::string& source, const std::string& baseDir) const {
        const PackageSource parsed = parse(source);
        if (parsed.type == "npm") {
            return "npm:" + npmName(parsed.npmSpec);
        }
        if (parsed.type == "git") {
            return "git:" + parsed.host + "/" + parsed.path;
        }
        return "local:" + m_paths.resolveToCwd(parsed.localPath, baseDir);
    }

    /** What settings store for a source: a local path relative to `baseDir`, anything else as written. */
    std::string settingsForm(const std::string& source, const std::string& baseDir) const {
        const PackageSource parsed = parse(source);
        if (parsed.type != "local") {
            return source;
        }
        const std::string resolved = m_paths.resolveToCwd(parsed.localPath, baseDir);
        const std::string relative = std::filesystem::path(resolved).lexically_relative(baseDir).generic_string();
        return relative.empty() ? "." : relative;
    }

    std::string resolveLocal(const std::string& path, const std::string& baseDir) const {
        return m_paths.resolveToCwd(trim(path), baseDir);
    }

private:
    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    GitSourceParser m_git;
    PathResolver m_paths;
};
