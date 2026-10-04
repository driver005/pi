export module pi.types.context;

import std;
export import pi.types.message;
export import pi.types.tool;

/** Request input of the public entry points; systemPrompt/tools fold into a system message. */
export struct Context {
    std::optional<std::string> systemPrompt;
    std::vector<Message> messages;
    std::optional<std::vector<Tool>> tools;
};
