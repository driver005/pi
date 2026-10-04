module;

#include <cstdint>

export module pi.durable.i_task_host;

import std;
export import pi.durable.i_conversation_agent;
export import pi.durable.i_conversation_handle;
export import pi.durable.i_execution_env;
export import pi.durable.i_registry_snapshot;
export import pi.provider.i_model_runtime;
export import pi.support.abort_signal;
export import pi.support.transaction;
export import pi.types.doc_address_args;
export import pi.types.doc_definition;
export import pi.types.error;
export import pi.types.json;
export import pi.types.resolved_settings;
export import pi.types.result;
export import pi.types.task_invocation;

/** The scheduler services one invocation's runtime delegates to; every call fails once the invocation ended. */
export class ITaskHost {
public:
    virtual ~ITaskHost() = default;

    virtual Result<void> commit(TaskInvocation& invocation,
                                const std::function<Result<std::optional<Json>>(Transaction&, const Json& current)>& change) = 0;
    virtual Result<std::optional<Json>> memo(TaskInvocation& invocation, const std::string& name) = 0;
    virtual Result<Json> memo(TaskInvocation& invocation, const std::string& name, const Json& candidate) = 0;
    virtual Result<std::optional<Json>> snapshot(TaskInvocation& invocation, const DocDefinition& definition,
                                                 const DocAddressArgs& args) = 0;
    virtual Result<std::optional<Json>> snapshotAsOf(TaskInvocation& invocation, const DocDefinition& definition,
                                                     const DocAddressArgs& args, std::int64_t entryId) = 0;
    virtual Result<std::optional<Json>> getTask(TaskInvocation& invocation, std::int64_t id) = 0;
    virtual Result<Json> waitForTask(TaskInvocation& invocation, std::int64_t id, const AbortSignal* cancel) = 0;
    virtual Result<std::vector<Json>> outcomes(TaskInvocation& invocation, const std::vector<std::int64_t>& ids) = 0;
    virtual Result<std::shared_ptr<IConversationHandle>> conversation(const std::shared_ptr<TaskInvocation>& invocation, std::int64_t id) = 0;
    virtual Result<std::optional<Json>> entry(TaskInvocation& invocation, std::int64_t id,
                                              const std::optional<std::string>& kind) = 0;
    virtual Result<Json> context(TaskInvocation& invocation, std::int64_t conversationId,
                                 const std::optional<std::int64_t>& at) = 0;
    virtual Result<std::shared_ptr<const IConversationAgent>> resolveAgent(TaskInvocation& invocation,
                                                               const std::shared_ptr<const IRegistrySnapshot>& snapshot) = 0;
    virtual ResolvedSettings settings() = 0;
    virtual IModelRuntime* models() = 0;
    virtual Result<std::shared_ptr<IExecutionEnv>> env(TaskInvocation& invocation) = 0;
    virtual std::int64_t now(TaskInvocation& invocation) = 0;
    virtual void report(TaskInvocation& invocation, const Error& error) = 0;
    virtual Result<void> sleep(TaskInvocation& invocation, std::int64_t until, const AbortSignal* cancel) = 0;
};
