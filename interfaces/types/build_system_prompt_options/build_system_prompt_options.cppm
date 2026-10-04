export module pi.types.build_system_prompt_options;

import std;
export import pi.types.context_file;
export import pi.types.skill;

/** Inputs of the system prompt. Port of BuildSystemPromptOptions in core/system-prompt.ts. */
export struct BuildSystemPromptOptions {
    /** Replaces the default preamble, tools, rules and docs sections. */
    std::optional<std::string> customPrompt;
    /** Exact full prompt replacement; opaque, with no sections. */
    std::optional<std::string> forceSystemPrompt;
    /** Tools shown in the prompt. */
    std::vector<std::string> selectedTools = {"read", "bash", "edit", "write"};
    /** One-line description per tool; tools without one are not listed. */
    std::map<std::string, std::string> toolSnippets;
    std::map<std::string, std::vector<std::string>> toolGuidelines;
    std::vector<std::string> promptGuidelines;
    std::string appendSystemPrompt;
    /** Extra XML-wrapped sections keyed by tag name (lowercase, [a-z0-9_-]). */
    std::vector<std::pair<std::string, std::string>> sections;
    std::string cwd;
    std::vector<ContextFile> contextFiles;
    std::vector<Skill> skills;
    /** Pi documentation locations; the docs section is omitted when readme is empty. */
    std::string readmePath;
    std::string docsPath;
    std::string examplesPath;
};
