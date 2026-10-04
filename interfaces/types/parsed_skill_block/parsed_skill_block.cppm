export module pi.types.parsed_skill_block;

import std;

/** A <skill> block found at the start of a user message, and the text that follows it. */
export struct ParsedSkillBlock {
    std::string name;
    std::string location;
    std::string content;
    std::optional<std::string> userMessage;
};
