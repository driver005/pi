module;
#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.assistant_message;

import std;
export import pi.types.assistant_content_block;
export import pi.types.deferred_handle;
export import pi.types.json;
export import pi.types.stop_reason;
export import pi.types.thinking_level;
export import pi.types.usage;

export struct AssistantMessage {
    std::vector<AssistantContentBlock> content;
    std::string api;
    std::string provider;
    std::string model;
    /** Concrete model reported by the provider when different from `model`. */
    std::optional<std::string> responseModel;
    std::optional<std::string> responseId;
    /** Exact provider-native effort level used. */
    std::optional<std::string> providerThinkingLevel;
    /** Pi thinking level the agent loop requested for this response. */
    std::optional<ThinkingLevel> thinkingLevel;
    /** Redacted provider/runtime diagnostics (array of objects); null means absent. */
    Json diagnostics;
    Usage usage;
    StopReason stopReason = StopReason::Stop;
    std::optional<DeferredHandle> deferred;
    std::optional<std::string> errorMessage;
    std::optional<std::string> rawStopReason;
    std::optional<bool> endTurn;
    std::int64_t timestamp = 0;
};
