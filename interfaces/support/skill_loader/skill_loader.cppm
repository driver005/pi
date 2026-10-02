module;

#include <nlohmann/json.hpp>

export module pi.support.skill_loader;

import std;
export import pi.platform.i_file_system;
export import pi.support.frontmatter_parser;
export import pi.support.layered_ignore;
export import pi.support.path_resolver;
export import pi.types.load_skills_options;
export import pi.types.load_skills_result;

/**
 * Discovers skills. A directory holding SKILL.md is a skill root and is not descended further;
 * otherwise its direct .md children and sub-directories are searched. .gitignore, .ignore and
 * .fdignore files are honored. Port of loadSkills in core/skills.ts.
 */
export class SkillLoader {
public:
    static constexpr std::size_t MaxNameLength = 64;
    static constexpr std::size_t MaxDescriptionLength = 1024;

    explicit SkillLoader(IFileSystem& files);

    LoadSkillsResult load(const LoadSkillsOptions& options);
    LoadSkillsResult loadFromDir(const std::string& dir, const std::string& source);

    std::vector<std::string> validateName(const std::string& name) const;

private:
    void scan(const std::string& dir, const std::string& source, bool includeRootFiles,
              LayeredIgnore& ignore, const std::string& root, LoadSkillsResult& out);
    bool loadSkillFile(const std::string& path, const std::string& source, LoadSkillsResult& out);
    void addIgnoreLayer(const std::string& dir, const std::string& root, LayeredIgnore& ignore);
    SourceInfo sourceInfo(const std::string& path, const std::string& baseDir, const std::string& source) const;
    std::string basename(const std::string& path) const;
    std::string dirname(const std::string& path) const;
    std::string relative(const std::string& path, const std::string& root) const;
    bool isUnder(const std::string& target, const std::string& root) const;
    bool endsWith(const std::string& text, const std::string& suffix) const;
    std::string trim(const std::string& text) const;

    IFileSystem& m_files;
    FrontmatterParser m_frontmatter;
    PathResolver m_paths;
};

SkillLoader::SkillLoader(IFileSystem& files) : m_files(files), m_paths(files.homeDirectory()) {}

std::string SkillLoader::basename(const std::string& path) const {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string SkillLoader::dirname(const std::string& path) const {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

std::string SkillLoader::relative(const std::string& path, const std::string& root) const {
    return m_paths.relativeTo(path, root);
}

bool SkillLoader::endsWith(const std::string& text, const std::string& suffix) const {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string SkillLoader::trim(const std::string& text) const {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool SkillLoader::isUnder(const std::string& target, const std::string& root) const {
    return target == root || target.rfind(root.back() == '/' ? root : root + "/", 0) == 0;
}

std::vector<std::string> SkillLoader::validateName(const std::string& name) const {
    std::vector<std::string> errors;
    if (name.size() > MaxNameLength) {
        errors.push_back("name exceeds " + std::to_string(MaxNameLength) + " characters (" +
                         std::to_string(name.size()) + ")");
    }
    const bool validChars = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
    if (!validChars) {
        errors.push_back("name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)");
    }
    if (!name.empty() && (name.front() == '-' || name.back() == '-')) {
        errors.push_back("name must not start or end with a hyphen");
    }
    if (name.find("--") != std::string::npos) {
        errors.push_back("name must not contain consecutive hyphens");
    }
    return errors;
}

SourceInfo SkillLoader::sourceInfo(const std::string& path, const std::string& baseDir,
                                   const std::string& source) const {
    SourceInfo info;
    info.path = path;
    info.baseDir = baseDir;
    if (source == "user" || source == "project") {
        info.source = "local";
        info.scope = source;
    } else if (source == "path") {
        info.source = "local";
    } else {
        info.source = source;
    }
    return info;
}

void SkillLoader::addIgnoreLayer(const std::string& dir, const std::string& root, LayeredIgnore& ignore) {
    const std::string relativeDir = dir == root ? "" : relative(dir, root);
    for (const char* name : {".gitignore", ".ignore", ".fdignore"}) {
        if (auto text = m_files.readFile(dir + "/" + name)) {
            ignore.addLayer(relativeDir, *text);
        }
    }
}

bool SkillLoader::loadSkillFile(const std::string& path, const std::string& source, LoadSkillsResult& out) {
    const bool declared = basename(path) == "SKILL.md";
    auto raw = m_files.readFile(path);
    if (!raw) {
        out.diagnostics.push_back(ResourceDiagnostic{"warning", raw.error().message, path, std::nullopt});
        return false;
    }
    auto document = m_frontmatter.parse(*raw);
    if (!document) {
        if (declared) {
            out.diagnostics.push_back(ResourceDiagnostic{"warning", document.error().message, path, std::nullopt});
        }
        return false;
    }
    const Json& frontmatter = document->frontmatter;
    const bool hasDescription = frontmatter.contains("description") && frontmatter["description"].is_string() &&
                                !trim(frontmatter["description"].get<std::string>()).empty();
    if (!declared && !hasDescription) {
        return false;
    }
    if (!hasDescription) {
        out.diagnostics.push_back(ResourceDiagnostic{"warning", "description is required", path, std::nullopt});
    } else if (frontmatter["description"].get<std::string>().size() > MaxDescriptionLength) {
        out.diagnostics.push_back(ResourceDiagnostic{
            "warning",
            "description exceeds " + std::to_string(MaxDescriptionLength) + " characters (" +
                std::to_string(frontmatter["description"].get<std::string>().size()) + ")",
            path, std::nullopt});
    }
    const std::string skillDir = dirname(path);
    std::string name = frontmatter.contains("name") && frontmatter["name"].is_string()
                           ? frontmatter["name"].get<std::string>()
                           : "";
    if (name.empty()) {
        name = basename(skillDir);
    }
    for (const auto& error : validateName(name)) {
        out.diagnostics.push_back(ResourceDiagnostic{"warning", error, path, std::nullopt});
    }
    if (!hasDescription) {
        return false;
    }
    Skill skill;
    skill.name = name;
    skill.description = frontmatter["description"].get<std::string>();
    skill.filePath = path;
    skill.baseDir = skillDir;
    skill.sourceInfo = sourceInfo(path, skillDir, source);
    skill.disableModelInvocation = frontmatter.contains("disable-model-invocation") &&
                                   frontmatter["disable-model-invocation"] == true;
    out.skills.push_back(std::move(skill));
    return true;
}

void SkillLoader::scan(const std::string& dir, const std::string& source, bool includeRootFiles,
                       LayeredIgnore& ignore, const std::string& root, LoadSkillsResult& out) {
    if (!m_files.exists(dir)) {
        return;
    }
    addIgnoreLayer(dir, root, ignore);
    auto names = m_files.listDirectory(dir);
    if (!names) {
        return;
    }
    std::sort(names->begin(), names->end());
    if (std::find(names->begin(), names->end(), "SKILL.md") != names->end()) {
        const std::string path = dir + "/SKILL.md";
        const auto info = m_files.stat(path);
        if (info && info->isFile && !ignore.ignores(relative(path, root), false)) {
            loadSkillFile(path, source, out);
            return;
        }
    }
    for (const auto& name : *names) {
        if (name.empty() || name[0] == '.' || name == "node_modules") {
            continue;
        }
        const std::string path = dir + "/" + name;
        const auto info = m_files.stat(path);
        if (!info) {
            continue;
        }
        if (ignore.ignores(relative(path, root), info->isDirectory)) {
            continue;
        }
        if (info->isDirectory) {
            scan(path, source, false, ignore, root, out);
        } else if (info->isFile && includeRootFiles && endsWith(name, ".md")) {
            loadSkillFile(path, source, out);
        }
    }
}

LoadSkillsResult SkillLoader::loadFromDir(const std::string& dir, const std::string& source) {
    LoadSkillsResult out;
    LayeredIgnore ignore;
    scan(dir, source, true, ignore, dir, out);
    return out;
}

LoadSkillsResult SkillLoader::load(const LoadSkillsOptions& options) {
    const std::string cwd = m_paths.resolveToCwd(options.cwd, "/");
    const std::string agentDir = m_paths.resolveToCwd(options.agentDir, "/");
    const std::string userDir = agentDir + "/skills";
    const std::string projectDir = cwd + "/.pi/skills";

    std::vector<Skill> ordered;
    std::map<std::string, std::size_t> byName;
    std::set<std::string> realPaths;
    LoadSkillsResult result;
    std::vector<ResourceDiagnostic> collisions;

    const auto add = [&](const LoadSkillsResult& found) {
        for (const auto& diagnostic : found.diagnostics) {
            result.diagnostics.push_back(diagnostic);
        }
        for (const auto& skill : found.skills) {
            const std::string real = m_files.realPath(skill.filePath);
            if (realPaths.contains(real)) {
                continue;
            }
            const auto existing = byName.find(skill.name);
            if (existing != byName.end()) {
                ResourceCollision collision;
                collision.resourceType = "skill";
                collision.name = skill.name;
                collision.winnerPath = ordered[existing->second].filePath;
                collision.loserPath = skill.filePath;
                collisions.push_back(ResourceDiagnostic{"collision", "name \"" + skill.name + "\" collision",
                                                        skill.filePath, collision});
            } else {
                byName[skill.name] = ordered.size();
                ordered.push_back(skill);
                realPaths.insert(real);
            }
        }
    };

    if (options.includeDefaults) {
        add(loadFromDir(userDir, "user"));
        add(loadFromDir(projectDir, "project"));
    }
    for (const auto& rawPath : options.skillPaths) {
        const std::string path = m_paths.resolveToCwd(trim(rawPath), cwd);
        if (!m_files.exists(path)) {
            result.diagnostics.push_back(ResourceDiagnostic{"warning", "skill path does not exist", path, std::nullopt});
            continue;
        }
        const auto info = m_files.stat(path);
        if (!info) {
            result.diagnostics.push_back(ResourceDiagnostic{"warning", info.error().message, path, std::nullopt});
            continue;
        }
        std::string source = "path";
        if (!options.includeDefaults) {
            if (isUnder(path, userDir)) {
                source = "user";
            } else if (isUnder(path, projectDir)) {
                source = "project";
            }
        }
        if (info->isDirectory) {
            add(loadFromDir(path, source));
        } else if (info->isFile && endsWith(path, ".md")) {
            LoadSkillsResult single;
            if (loadSkillFile(path, source, single)) {
                add(single);
            } else {
                for (const auto& diagnostic : single.diagnostics) {
                    result.diagnostics.push_back(diagnostic);
                }
            }
        } else {
            result.diagnostics.push_back(
                ResourceDiagnostic{"warning", "skill path is not a markdown file", path, std::nullopt});
        }
    }
    result.skills = std::move(ordered);
    for (auto& collision : collisions) {
        result.diagnostics.push_back(std::move(collision));
    }
    return result;
}
