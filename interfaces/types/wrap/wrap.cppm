export module pi.types.wrap;

import std;
export import pi.types.prompt_section;
export import pi.types.tool_registration;

/** Rewrites the tool or section it targets wherever the wrapping extension is selected; wrappers are pure. */
export struct Wrap {
    /** "tool" or "section". */
    std::string kind = "tool";
    /** The tool name or section key. */
    std::string target;
    std::function<ToolRegistration(const ToolRegistration& tool)> wrapTool;
    std::function<PromptSection(const PromptSection& section)> wrapSection;
};
