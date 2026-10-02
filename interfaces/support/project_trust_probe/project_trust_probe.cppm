export module pi.support.project_trust_probe;

import std;
export import pi.platform.i_file_system;
export import pi.support.path_resolver;

/**
 * Whether a working directory holds project-local resources that need a trust decision:
 * <cwd>/.pi/{settings.json, mcp.json, extensions, skills, prompts, themes, SYSTEM.md,
 * APPEND_SYSTEM.md} or an .agents/skills directory in the cwd or an ancestor (the user's own
 * ~/.agents/skills does not count). Port of hasTrustRequiringProjectResources in core/trust-manager.ts.
 */
export class ProjectTrustProbe {
public:
    explicit ProjectTrustProbe(IFileSystem& files);

    bool requiresTrust(const std::string& cwd);

    /** Candidate directory used as the trust key: resolved and canonical. */
    std::string trustPath(const std::string& cwd);

private:
    IFileSystem& m_files;
    PathResolver m_paths;
};

ProjectTrustProbe::ProjectTrustProbe(IFileSystem& files) : m_files(files), m_paths(files.homeDirectory()) {}

std::string ProjectTrustProbe::trustPath(const std::string& cwd) {
    return m_files.realPath(m_paths.resolveToCwd(cwd, "/"));
}

bool ProjectTrustProbe::requiresTrust(const std::string& cwd) {
    const std::string home = m_files.realPath(m_files.homeDirectory());
    const std::string userSkills = home + "/.agents/skills";
    std::string current = trustPath(cwd);
    for (const char* entry : {"settings.json", "mcp.json", "extensions", "skills", "prompts", "themes",
                              "SYSTEM.md", "APPEND_SYSTEM.md"}) {
        if (m_files.exists(current + "/.pi/" + entry)) {
            return true;
        }
    }
    while (true) {
        const std::string skills = (current == "/" ? "" : current) + "/.agents/skills";
        if (skills != userSkills && m_files.exists(skills)) {
            return true;
        }
        const auto slash = current.find_last_of('/');
        const std::string parent = slash == std::string::npos || slash == 0 ? "/" : current.substr(0, slash);
        if (parent == current) {
            return false;
        }
        current = parent;
    }
}
