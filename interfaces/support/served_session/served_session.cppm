export module pi.support.served_session;

import std;
export import pi.platform.i_executor;
export import pi.platform.i_id_generator;
export import pi.provider.i_model_runtime;
export import pi.server.i_routed_session_handle;
export import pi.session.i_session_runtime_handle;
import pi.support.agent_controller_service;
import pi.support.models_service;
import pi.support.provider_service_attachment;
import pi.support.remote_service_provider;
import pi.support.transcript_service;

/**
 * One live session offered to connections: the agent controller, models and transcript services
 * of the session, shared by every attached client. Closing disposes the services, stops the run and
 * leaves the session ready to be destroyed.
 */
export class ServedSession : public IRoutedSessionHandle {
public:
    ServedSession(std::unique_ptr<ISessionRuntimeHandle> runtime, IModelRuntime& models, IExecutor& executor,
                  IIdGenerator& ids);
    ~ServedSession() override;

    ServedSession(const ServedSession&) = delete;
    ServedSession& operator=(const ServedSession&) = delete;

    Result<std::unique_ptr<IServiceAttachment>> attachClient(const ServiceContext& context) override;
    void onTermination(TerminationListener listener) override;
    Result<void> close(const ServiceContext& context) override;

    ISessionRuntimeHandle& runtime();

private:
    std::unique_ptr<ISessionRuntimeHandle> m_runtime;
    std::shared_ptr<AgentControllerService> m_controller;
    std::shared_ptr<ModelsService> m_models;
    std::shared_ptr<TranscriptService> m_transcript;
    std::shared_ptr<RemoteServiceProvider> m_provider;
    std::mutex m_mutex;
    bool m_closed = false;
};

ServedSession::ServedSession(std::unique_ptr<ISessionRuntimeHandle> runtime, IModelRuntime& models,
                             IExecutor& executor, IIdGenerator& ids)
    : m_runtime(std::move(runtime)),
      m_controller(std::make_shared<AgentControllerService>(m_runtime->session(), executor, ids)),
      m_models(std::make_shared<ModelsService>(m_runtime->session(), models)),
      m_transcript(std::make_shared<TranscriptService>(m_runtime->session())),
      m_provider(std::make_shared<RemoteServiceProvider>(std::vector<ServiceDefinition>{
          {"pi.agent-controller", "singleton"}, {"pi.models", "singleton"}, {"pi.transcript", "singleton"}})) {
    m_provider->provide("pi.agent-controller", m_controller);
    m_provider->provide("pi.models", m_models);
    m_provider->provide("pi.transcript", m_transcript);
}

ServedSession::~ServedSession() {
    close(ServiceContext{std::make_shared<AbortSignal>()});
}

ISessionRuntimeHandle& ServedSession::runtime() {
    return *m_runtime;
}

Result<std::unique_ptr<IServiceAttachment>> ServedSession::attachClient(const ServiceContext&) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(Error{"server_draining", "Session is closed"});
        }
    }
    return std::unique_ptr<IServiceAttachment>(
        std::make_unique<ProviderServiceAttachment>(m_provider, std::function<void()>()));
}

void ServedSession::onTermination(TerminationListener) {}

Result<void> ServedSession::close(const ServiceContext&) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return {};
        }
        m_closed = true;
    }
    m_provider->dispose();
    IAgentSession& session = m_runtime->session();
    session.clearQueue();
    session.abort();
    session.waitForIdle();
    return {};
}
