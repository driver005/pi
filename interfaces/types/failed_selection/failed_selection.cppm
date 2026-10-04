export module pi.types.failed_selection;

import std;
export import pi.types.assistant_message;
export import pi.types.model;
export import pi.types.thinking_level;

/** The request that failed before a retry: its physical model and thinking level, and the failed assistant message. */
export struct FailedSelection {
    Model model;
    std::optional<ThinkingLevel> thinkingLevel;
    AssistantMessage message;
};
