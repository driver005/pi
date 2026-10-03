export module pi.support.durable_served_session;

import std;
export import pi.provider.i_model_runtime;
export import pi.server.i_routed_session_handle;
export import pi.support.durable_agent_controller_service;
export import pi.support.durable_models_service;
export import pi.support.durable_transcript_service;
export import pi.support.harness;
export import pi.support.registry;
import pi.support.provider_service_attachment;
import pi.support.remote_service_provider;
import pi.support.session_plugins_service;

/**
 * One live durable session offered to connections: the agent controller, models and transcript services of the root
 * conversation of a harness, shared by every attached client, plus `pi.session-plugins` when the host can reload its plugins. It owns the harness and the registry the harness reads, and
 * closing it disposes the services, aborts the run and closes the harness (and with it the storage).
 */
export class DurableServedSession : public IRoutedSessionHandle {
public:
    /** `onClosed` runs once when the session closes, before the services and the registry go. */
    DurableServedSession(std::shared_ptr<Registry> registry, std::unique_ptr<Harness> harness, std::shared_ptr<Conversation> root, IModelRuntime& models, DurableModelsService::SelectedListener onSelected,
                         std::function<void()> onClosed = nullptr, SessionPluginsService::Reload reloadPlugins = nullptr)
        : m_registry(std::move(registry)),
          m_onClosed(std::move(onClosed)),
          m_harness(std::move(harness)),
          m_root(std::move(root)),
          m_provider(std::make_shared<RemoteServiceProvider>(definitions(static_cast<bool>(reloadPlugins)))) {
        auto view = m_harness->viewState(m_root->id());
        if (view) {
            m_provider->provide("pi.agent-controller", std::make_shared<DurableAgentControllerService>(*m_harness, m_root));
            m_provider->provide("pi.models", std::make_shared<DurableModelsService>(m_root, *view, models, std::move(onSelected)));
            m_provider->provide("pi.transcript", std::make_shared<DurableTranscriptService>(*view));
            if (reloadPlugins) {
                m_provider->provide("pi.session-plugins", std::make_shared<SessionPluginsService>(std::move(reloadPlugins)));
            }
        }
    }

    ~DurableServedSession() override {
        (void)close(ServiceContext{std::make_shared<AbortSignal>()});
    }

    DurableServedSession(const DurableServedSession&) = delete;
    DurableServedSession& operator=(const DurableServedSession&) = delete;

    Result<std::unique_ptr<IServiceAttachment>> attachClient(const ServiceContext&) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return std::unexpected(Error{"server_draining", "Session is closed"});
            }
        }
        return std::unique_ptr<IServiceAttachment>(std::make_unique<ProviderServiceAttachment>(m_provider, std::function<void()>()));
    }

    void onTermination(TerminationListener) override {}

    Result<void> close(const ServiceContext&) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return {};
            }
            m_closed = true;
        }
        if (m_onClosed) {
            m_onClosed();
        }
        // Stop the work first: the services outlive every publication the running tasks still deliver to them.
        (void)m_root->abort();
        auto closed = m_harness->close();
        m_provider->dispose();
        return closed;
    }

private:
    std::vector<ServiceDefinition> definitions(bool withPlugins) const {
        std::vector<ServiceDefinition> found{{"pi.agent-controller", "singleton"}, {"pi.models", "singleton"}, {"pi.transcript", "singleton"}};
        if (withPlugins) {
            found.push_back({"pi.session-plugins", "singleton"});
        }
        return found;
    }

    std::shared_ptr<Registry> m_registry;
    std::function<void()> m_onClosed;
    std::unique_ptr<Harness> m_harness;
    std::shared_ptr<Conversation> m_root;
    std::shared_ptr<RemoteServiceProvider> m_provider;
    std::mutex m_mutex;
    bool m_closed = false;
};
