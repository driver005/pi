export module pi.types.extension;

import std;
export import pi.durable.task_definition;
export import pi.types.hook_registration;
export import pi.types.prompt_section;
export import pi.types.tool_registration;
export import pi.types.wrap;

/** A named bundle of code, installed in a registry and selected by conversations by name. */
export struct Extension {
    std::string name;
    std::vector<ToolRegistration> tools;
    std::vector<PromptSection> sections;
    std::vector<HookRegistration> hooks;
    /** Applied where this extension is selected, in order. */
    std::vector<Wrap> wraps;
    /** Resolved by name for every task, whichever conversations select this extension. */
    std::vector<std::shared_ptr<const TaskDefinition>> tasks;
};
