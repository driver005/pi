export module pi.types.model_choice;

import std;
export import pi.types.model;
export import pi.types.thinking_level;

/** The model and thinking level a session starts with; no model when none is usable. */
export struct ModelChoice {
    std::optional<Model> model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::vector<std::string> warnings;
};
