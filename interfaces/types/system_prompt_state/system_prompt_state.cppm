export module pi.types.system_prompt_state;

import std;

/**
 * The prompt as a transcript system message holds it: a forced prompt is opaque `content` with
 * no sections; otherwise `content` is empty and the ordered sections carry the prompt.
 */
export struct SystemPromptState {
    std::string content;
    std::optional<std::vector<std::pair<std::string, std::string>>> sections;
};
