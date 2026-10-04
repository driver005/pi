export module pi.types.routed_selection;

import std;
export import pi.types.model;
export import pi.types.thinking_level;

/** A physical model and the thinking level a request to it used. */
export struct RoutedSelection {
    Model model;
    std::optional<ThinkingLevel> thinkingLevel;
};
