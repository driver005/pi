export module pi.types.skill;

import std;
export import pi.types.source_info;

/** A SKILL.md (or standalone .md) that tells the model when and how to use a capability. */
export struct Skill {
    std::string name;
    std::string description;
    std::string filePath;
    std::string baseDir;
    SourceInfo sourceInfo;
    /** Hidden from the system prompt; only reachable through /skill:name. */
    bool disableModelInvocation = false;
};
