module;

#include <cstdint>

export module pi.types.agent_session_event;

import std;
export import pi.types.agent_event;
export import pi.types.compaction_result;
export import pi.types.session_entry;
export import pi.types.thinking_level;

export enum class SessionEventType {
    /** An agent loop event (message, turn, tool execution); `agent` is set. */
    Agent,
    /** The run ended; `agent` holds the AgentEnd event and `willRetry` says whether a retry follows. */
    AgentEnd,
    /** The whole prompt, including retries and compaction continuations, finished. */
    AgentSettled,
    QueueUpdate,
    CompactionStart,
    CompactionEnd,
    EntryAppended,
    SessionInfoChanged,
    ThinkingLevelChanged,
    AutoRetryStart,
    AutoRetryEnd,
    SummarizationRetryScheduled,
    SummarizationRetryAttemptStart,
    SummarizationRetryFinished,
    BashExecutionUpdate
};

/**
 * Event of an agent session. Which fields are set depends on `type`:
 * Agent/AgentEnd: agent (+ willRetry). QueueUpdate: steering, followUp. CompactionStart:
 * reason. CompactionEnd: reason, result, aborted, willRetry, errorMessage. EntryAppended: entry.
 * SessionInfoChanged: name. ThinkingLevelChanged: level. AutoRetryStart: attempt, maxAttempts,
 * delayMs, errorMessage. AutoRetryEnd: success, attempt, finalError. SummarizationRetryScheduled:
 * attempt, maxAttempts, delayMs, errorMessage. SummarizationRetryAttemptStart: source (and reason
 * for compaction). BashExecutionUpdate: id, delta.
 */
export struct AgentSessionEvent {
    SessionEventType type = SessionEventType::Agent;
    std::shared_ptr<const AgentEvent> agent;
    bool willRetry = false;
    std::vector<std::string> steering;
    std::vector<std::string> followUp;
    /** Compaction trigger: "manual", "threshold" or "overflow". */
    std::string reason;
    /** Summarization source: "compaction" or "branchSummary". */
    std::string source;
    std::optional<CompactionResult> result;
    bool aborted = false;
    std::optional<std::string> errorMessage;
    int attempt = 0;
    int maxAttempts = 0;
    std::int64_t delayMs = 0;
    bool success = false;
    std::optional<std::string> finalError;
    std::optional<SessionEntry> entry;
    std::optional<std::string> name;
    ThinkingLevel level = ThinkingLevel::Off;
    std::optional<std::string> id;
    std::string delta;
};
