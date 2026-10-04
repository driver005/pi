export module pi.types.slash_command_info;

import std;
export import pi.types.source_info;

/** A command a prompt can invoke by name: a prompt template or a skill (plugins add more later). */
export struct SlashCommandInfo {
    /** Without the leading slash; skills are "skill:<name>". */
    std::string name;
    std::string description;
    /** "extension" | "prompt" | "skill". */
    std::string source;
    SourceInfo sourceInfo;
};
