module;

#include <cstdint>

export module pi.types.compaction_check;

import std;
export import pi.types.agent_message;
export import pi.types.assistant_message;
export import pi.types.compaction_settings;
export import pi.types.session_entry;
export import pi.types.session_projection;

/** Inputs of one automatic-compaction decision (references stay valid for the call). */
export struct CompactionCheck {
    const AssistantMessage& assistant;
    /** Treat an aborted response as not worth compacting for (post-run); false before a prompt. */
    bool skipAbortedCheck;
    const CompactionSettings& settings;
    /** The response came from the currently selected model, so its limits apply. */
    bool sameModel;
    std::int64_t contextWindow;
    /** The model's configured maximum output, for recoverable length stops. */
    std::int64_t maxOutputTokens;
    /** Session entry holding the response, when known. */
    const std::optional<std::string>& assistantEntryId;
    const std::vector<SessionEntry>& branch;
    const SessionProjection& projection;
    /** Agent state messages (finalized transcript). */
    const std::vector<AgentMessage>& messages;
    bool overflowRecoveryAttempted;
};
