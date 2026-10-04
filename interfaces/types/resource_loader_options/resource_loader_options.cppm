export module pi.types.resource_loader_options;

import std;

/** What a resource loader reads, beyond what settings name. */
export struct ResourceLoaderOptions {
    std::string cwd;
    std::string agentDir;
    /** Replaces the discovered SYSTEM.md: a file path (its content is used) or literal text. */
    std::optional<std::string> systemPrompt;
    /** Replaces the discovered APPEND_SYSTEM.md; each entry is a file path or literal text. */
    std::optional<std::vector<std::string>> appendSystemPrompt;
    bool noContextFiles = false;
    /** Skip default skill directories and settings paths; only additionalSkillPaths load. */
    bool noSkills = false;
    bool noPromptTemplates = false;
    std::vector<std::string> additionalSkillPaths;
    std::vector<std::string> additionalPromptTemplatePaths;
};
