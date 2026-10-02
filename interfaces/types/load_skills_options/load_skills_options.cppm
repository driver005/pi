export module pi.types.load_skills_options;

import std;

/** Where to look for skills. */
export struct LoadSkillsOptions {
    /** Working directory for project-local skills (<cwd>/.pi/skills). */
    std::string cwd;
    /** Agent directory for user skills (<agentDir>/skills). */
    std::string agentDir;
    /** Explicit skill files or directories. */
    std::vector<std::string> skillPaths;
    /** Also scan the default user and project skill directories. */
    bool includeDefaults = true;
};
