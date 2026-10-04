export module pi.types.resource_collision;

import std;

/** Two resources claimed the same name; the first one loaded wins. */
export struct ResourceCollision {
    /** "extension" | "skill" | "prompt" | "theme". */
    std::string resourceType;
    std::string name;
    std::string winnerPath;
    std::string loserPath;
    std::optional<std::string> winnerSource;
    std::optional<std::string> loserSource;
};
