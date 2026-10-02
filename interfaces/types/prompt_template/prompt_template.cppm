export module pi.types.prompt_template;

import std;
export import pi.types.source_info;

/** A /name command whose body is expanded into the prompt, with $1 / $@ argument placeholders. */
export struct PromptTemplate {
    std::string name;
    std::string description;
    std::optional<std::string> argumentHint;
    std::string content;
    SourceInfo sourceInfo;
    std::string filePath;
};
