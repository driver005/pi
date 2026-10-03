module;

#include <cstdint>

export module pi.types.scheduler_callbacks;

import std;
export import pi.durable.i_agent;
export import pi.durable.i_conversation_handle;
export import pi.durable.i_execution_env;
export import pi.durable.i_registry_snapshot;
export import pi.provider.i_model_runtime;
export import pi.support.transaction;
export import pi.types.error;
export import pi.types.json;
export import pi.types.resolved_settings;
export import pi.types.result;
export import pi.types.task_invocation;

/** What the task scheduler needs from the harness around it. */
export struct SchedulerCallbacks {
    /** Resolves a conversation's agent against a snapshot; the runtime calls it at most once per phase. */
    std::function<Result<std::shared_ptr<const IAgent>>(std::int64_t conversationId, const std::shared_ptr<const IRegistrySnapshot>&)> agent;
    /** The settings, read at each access. */
    std::function<ResolvedSettings()> settings;
    /** The model catalog the built-in tasks call; may be absent (then they fail with no_model). */
    std::function<IModelRuntime*()> models;
    /** Builds a conversation's environment; null without one. */
    std::function<Result<std::shared_ptr<IExecutionEnv>>(std::int64_t conversationId)> env;
    std::function<std::int64_t()> now;
    /** Receives failures that do not fail the calling operation; must not throw. */
    std::function<void(const Error&)> report;
    /** Harness cleanup staged in the commit that makes an outcome the scheduler wrote itself terminal. */
    std::function<Result<void>(Transaction&, const Json& record, const Json& outcome)> settleOutcome;
    /** Withdraws a conversation's queued inputs, for conversation abort and abort cascades. */
    std::function<Result<void>(Transaction&, std::int64_t conversationId)> withdrawInputs;
    /** An invocation-bound handle of an existing conversation; null when absent. */
    std::function<Result<std::shared_ptr<IConversationHandle>>(std::int64_t id, TaskInvocation& binding)> conversation;
    /** The committed raw active transcript and model context of a conversation, optionally cut off at an entry. */
    std::function<Result<Json>(std::int64_t conversationId, const std::optional<std::int64_t>& at)> context;
};
