export module pi.types.load_prompt_templates_options;

import std;

/** Where to look for prompt templates. */
export struct LoadPromptTemplatesOptions {
    std::string cwd;
    std::string agentDir;
    /** Explicit template files or directories. */
    std::vector<std::string> promptPaths;
    /** Also scan <agentDir>/prompts and <cwd>/.pi/prompts. */
    bool includeDefaults = true;
    /** With includeDefaults, also scan <cwd>/.pi/prompts (off when the project is not trusted). */
    bool includeProject = true;
};
