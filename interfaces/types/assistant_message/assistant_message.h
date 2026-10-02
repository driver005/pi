#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "interfaces/types/assistant_content_block/assistant_content_block.h"
#include "interfaces/types/deferred_handle/deferred_handle.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/stop_reason/stop_reason.h"
#include "interfaces/types/thinking_level/thinking_level.h"
#include "interfaces/types/usage/usage.h"

struct AssistantMessage {
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
