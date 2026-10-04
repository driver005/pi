export module pi.types.source_info;

import std;

/** Where a resource (skill, prompt, extension) came from. */
export struct SourceInfo {
    std::string path;
    /** "local", "builtin", "npm:...", "git:...", "inline", ... */
    std::string source;
    /** "user" | "project" | "temporary". */
    std::string scope = "temporary";
    /** "package" | "top-level". */
    std::string origin = "top-level";
    std::optional<std::string> baseDir;
};
