export module pi.types.resource_diagnostic;

import std;
export import pi.types.resource_collision;

/** A problem found while loading resources; loading continues. */
export struct ResourceDiagnostic {
    /** "warning" | "error" | "collision". */
    std::string type;
    std::string message;
    std::optional<std::string> path;
    std::optional<ResourceCollision> collision;
};
