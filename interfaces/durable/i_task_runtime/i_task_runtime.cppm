module;

#include <cstdint>

export module pi.durable.i_task_runtime;

import std;
export import pi.durable.i_conversation_agent;
export import pi.durable.i_conversation_handle;
export import pi.durable.i_execution_env;
export import pi.durable.i_hook_api;
export import pi.provider.i_model_runtime;
export import pi.support.abort_signal;
export import pi.support.transaction;
export import pi.types.error;
export import pi.types.hook_handler;
export import pi.types.json;
export import pi.types.resolved_settings;
export import pi.types.result;

/**
 * Operations of one task invocation. Every operation fails once the invocation ends; the signal is aborted when the
 * run is signalled (`abortTask`), the harness closes, or the invocation ends.
 */
export class ITaskRuntime : public IHookApi {
public:
    virtual AbortSignal& signal() = 0;
    /** The conversation's agent, resolved at most once per phase, at first use, and fixed for the phase. */
    virtual Result<std::shared_ptr<const IConversationAgent>> agent() = 0;
    /** The model catalog and request-time authentication; null when the harness has none. */
    virtual IModelRuntime* models() = 0;
    /** The run settings, resolved at each access. */
    virtual ResolvedSettings settings() = 0;
    /** The conversation's environment, built at each call; null without an environment. */
    virtual Result<std::shared_ptr<IExecutionEnv>> env() = 0;
    /** Calls `invoke` with each handler of hook `name` from the selected extensions; failures are reported. */
    virtual Result<void> eachHook(const std::string& name, const std::function<Result<void>(const HookHandler&)>& invoke) = 0;

    /**
     * Commits on the session line after rereading the task. Fails when the task is terminal, the invocation ended, the
     * harness is closing, or (in a run invocation) the task carries an abort mark. A returned state
     * (`{status: "running"|"waiting"|"terminal", ...}`) replaces the task's state in the same commit; nothing leaves it.
     */
    virtual Result<void> commit(const std::function<Result<std::optional<Json>>(Transaction& tx, const Json& current)>& change) = 0;

    /**
     * `{version, checkpoint}` of a new task of a registered definition, from the phase's registry snapshot; fails when no
     * definition of that name is registered. Pair it with `Transaction::createTask`.
     */
    virtual Result<Json> newTask(const std::string& name, const Json& input) = 0;

    virtual Result<std::optional<Json>> getTask(std::int64_t id) = 0;
    /** Blocks until the task is terminal and returns its record; fails when the invocation ends. */
    virtual Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) = 0;
    /** Outcomes of terminal tasks, in order; fails when one is missing or not terminal. */
    virtual Result<std::vector<Json>> outcomes(const std::vector<std::int64_t>& ids) = 0;
    /** An invocation-bound handle of an existing conversation; null when absent. */
    virtual Result<std::shared_ptr<IConversationHandle>> conversation(std::int64_t id) = 0;
    /** A committed entry visible from the task's conversation, optionally only of one kind. */
    virtual Result<std::optional<Json>> entry(std::int64_t id, const std::optional<std::string>& kind = std::nullopt) = 0;
    /**
     * The committed raw active transcript and model context of a conversation, optionally cut off at the visible
     * entry `at`: `{head, entries, contributions, messages}`.
     */
    virtual Result<Json> context(std::int64_t conversationId, const std::optional<std::int64_t>& at = std::nullopt) = 0;
    /** The harness clock in milliseconds. */
    virtual std::int64_t now() = 0;
    /** Forwards a non-fatal failure to the host. */
    virtual void report(const Error& error) = 0;
    /** Blocks until the harness clock reaches `until`; fails when the invocation or `cancel` is aborted. */
    virtual Result<void> sleep(std::int64_t until, const AbortSignal* cancel = nullptr) = 0;
};
