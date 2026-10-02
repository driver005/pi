export module pi.types.model_cycle_result;

import std;
export import pi.types.model;
export import pi.types.thinking_level;

/** Outcome of cycling the model: the new model and level, and whether the scoped list was used. */
export struct ModelCycleResult {
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    bool isScoped = false;
};
