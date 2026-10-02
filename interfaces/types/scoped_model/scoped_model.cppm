export module pi.types.scoped_model;

import std;
export import pi.types.model;
export import pi.types.thinking_level;

/** A model in the cycling scope (the --models list) with an optional fixed thinking level. */
export struct ScopedModel {
    Model model;
    std::optional<ThinkingLevel> thinkingLevel;
};
