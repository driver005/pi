module;

#include <cstdint>

export module pi.durable.i_tool_execution_api;

import std;
export import pi.durable.i_execution_env;
export import pi.durable.i_hook_api;
export import pi.support.abort_signal;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;
export import pi.types.task_options;
export import pi.types.tool_diagnostic;

/** Operations available to one tool invocation; every operation fails once the invocation ends. */
export class IToolExecutionApi : public IHookApi {
public:
    virtual std::string callId() const = 0;
    /** The conversation's environment, built for this call; null without an environment. */
    virtual std::shared_ptr<IExecutionEnv> env() = 0;
    /** Appends running output; it becomes the result content when the result omits `content`. */
    virtual Result<void> output(const std::string& chunk) = 0;
    /** Records a model-visible remark about this call. */
    virtual Result<void> diagnostic(const ToolDiagnostic& diagnostic) = 0;
    /** Replaces the running details and waits for them to commit; the last value becomes the result details. */
    virtual Result<void> details(const Json& value) = 0;
    /** Commits on the session line in the call's conversation. */
    virtual Result<void> commit(const std::function<Result<void>(Transaction&)>& change) = 0;
    /** Creates a task of a registered definition in this call's conversation. */
    virtual Result<std::int64_t> createTask(const std::string& name, const Json& input, const TaskOptions& options) = 0;
    virtual Result<std::optional<Json>> getTask(std::int64_t id) = 0;
    /** Blocks until the task is terminal; fails when the invocation ends or `cancel` aborts. */
    virtual Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) = 0;
    /** The signal aborted when the call is cancelled. */
    virtual AbortSignal& signal() = 0;
};
