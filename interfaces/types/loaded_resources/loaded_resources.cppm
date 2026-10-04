export module pi.types.loaded_resources;

import std;
export import pi.types.context_file;
export import pi.types.prompt_template;
export import pi.types.resource_diagnostic;
export import pi.types.skill;

/** Everything a session reads from disk besides its own files: instructions, skills, templates. */
export struct LoadedResources {
    std::vector<Skill> skills;
    std::vector<PromptTemplate> promptTemplates;
    std::vector<ContextFile> contextFiles;
    /** SYSTEM.md content replacing the default prompt, when present. */
    std::optional<std::string> systemPrompt;
    /** APPEND_SYSTEM.md contents, in discovery order. */
    std::vector<std::string> appendSystemPrompt;
    std::vector<ResourceDiagnostic> diagnostics;
};
