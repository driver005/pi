export module pi.types.prompt_section;

import std;
export import pi.types.prompt_input;
export import pi.types.result;

/** One system prompt section; the agent's sections render in order before each request. */
export struct PromptSection {
    std::string key;
    /** The text, or nothing to omit the section. */
    std::function<Result<std::optional<std::string>>(const PromptInput& input)> render;
    /** Default true: wrap the text as `<key>\n...\n</key>`. */
    bool tag = true;
};
