module;

#include <cstdint>

export module pi.support.invocation_runtime;

import std;
export import pi.durable.i_task_host;
export import pi.durable.i_task_runtime;
export import pi.types.task_invocation;

/**
 * The ITaskRuntime of one invocation: a thin binding of the invocation to its scheduler's services. It
 * owns the phase state the TS runtime closes over: the registry snapshot the phase sees and its lazily
 * resolved agent, fixed until the next phase boundary.
 */
export class InvocationRuntime : public ITaskRuntime {
public:
    InvocationRuntime(ITaskHost& host, std::shared_ptr<TaskInvocation> invocation, std::string taskName,
                      std::function<std::shared_ptr<const IRegistrySnapshot>()> snapshot)
        : m_host(host),
          m_invocation(std::move(invocation)),
          m_taskName(std::move(taskName)),
          m_snapshot(std::move(snapshot)) {}

    /** Forgets the phase's agent: each phase handler resolves it afresh, at first use. */
    void resetPhase() {
        m_agent.reset();
    }

    /** The task definition name hooks are matched by; follows a definition handover. */
    void renameTask(const std::string& taskName) {
        m_taskName = taskName;
    }

    std::int64_t taskId() const override {
        return m_invocation->taskId;
    }

    std::int64_t conversationId() const override {
        return m_invocation->conversationId;
    }

    AbortSignal& signal() override {
        return m_invocation->signal;
    }

    Result<std::shared_ptr<const IConversationAgent>> agent() override {
        if (m_invocation->ended) {
            return std::unexpected(ended());
        }
        if (!m_agent) {
            auto resolved = m_host.resolveAgent(*m_invocation, m_snapshot());
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            m_agent = *resolved;
        }
        return m_agent;
    }

    ResolvedSettings settings() override {
        return m_host.settings();
    }

    IModelRuntime* models() override {
        return m_host.models();
    }

    Result<std::shared_ptr<IExecutionEnv>> env() override {
        return m_host.env(*m_invocation);
    }

    Result<void> eachHook(const std::string& name, const std::function<Result<void>(const HookHandler&)>& invoke) override {
        auto resolved = agent();
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        for (const HookRegistration& registration : (*resolved)->hooks(m_taskName)) {
            auto handler = registration.handlers.find(name);
            if (handler == registration.handlers.end() || !handler->second) {
                continue;
            }
            if (auto invoked = invoke(handler->second); !invoked) {
                if (m_invocation->signal.aborted()) {
                    return invoked;
                }
                m_host.report(*m_invocation, invoked.error());
            }
        }
        return {};
    }

    Result<void> commit(const std::function<Result<std::optional<Json>>(Transaction&, const Json&)>& change) override {
        return m_host.commit(*m_invocation, change);
    }

    Result<std::optional<Json>> memo(const std::string& name) override {
        return m_host.memo(*m_invocation, name);
    }

    Result<Json> memo(const std::string& name, const Json& candidate) override {
        return m_host.memo(*m_invocation, name, candidate);
    }

    Result<std::optional<Json>> snapshot(const DocDefinition& definition, const DocAddressArgs& args) override {
        return m_host.snapshot(*m_invocation, definition, args);
    }

    Result<std::optional<Json>> snapshotAsOf(const DocDefinition& definition, const DocAddressArgs& args,
                                             std::int64_t entryId) override {
        return m_host.snapshotAsOf(*m_invocation, definition, args, entryId);
    }

    Result<Json> newTask(const std::string& name, const Json& input) override {
        auto snapshot = m_snapshot();
        auto definition = snapshot ? snapshot->task(name) : nullptr;
        if (!definition || !definition->initial) {
            return std::unexpected(Error{"missing_task", "Task " + name + " is not registered"});
        }
        return Json::object({{"version", definition->version}, {"checkpoint", definition->initial(input)}});
    }

    Result<std::optional<Json>> getTask(std::int64_t id) override {
        return m_host.getTask(*m_invocation, id);
    }

    Result<Json> waitForTask(std::int64_t id, const AbortSignal* cancel = nullptr) override {
        return m_host.waitForTask(*m_invocation, id, cancel);
    }

    Result<std::vector<Json>> outcomes(const std::vector<std::int64_t>& ids) override {
        return m_host.outcomes(*m_invocation, ids);
    }

    Result<std::shared_ptr<IConversationHandle>> conversation(std::int64_t id) override {
        return m_host.conversation(m_invocation, id);
    }

    Result<std::optional<Json>> entry(std::int64_t id, const std::optional<std::string>& kind = std::nullopt) override {
        return m_host.entry(*m_invocation, id, kind);
    }

    Result<Json> context(std::int64_t conversationId, const std::optional<std::int64_t>& at = std::nullopt) override {
        return m_host.context(*m_invocation, conversationId, at);
    }

    std::int64_t now() override {
        return m_host.now(*m_invocation);
    }

    void report(const Error& error) override {
        m_host.report(*m_invocation, error);
    }

    Result<void> sleep(std::int64_t until, const AbortSignal* cancel = nullptr) override {
        return m_host.sleep(*m_invocation, until, cancel);
    }

private:
    Error ended() const {
        return Error{"invocation_ended", "Task " + std::to_string(m_invocation->taskId) + " invocation has ended"};
    }

    ITaskHost& m_host;
    std::shared_ptr<TaskInvocation> m_invocation;
    std::string m_taskName;
    std::function<std::shared_ptr<const IRegistrySnapshot>()> m_snapshot;
    std::shared_ptr<const IConversationAgent> m_agent;
};
