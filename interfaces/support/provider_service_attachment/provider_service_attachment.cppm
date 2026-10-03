export module pi.support.provider_service_attachment;

import std;
export import pi.chord.i_service_provider;
export import pi.server.i_service_attachment;
import pi.support.remote_service_endpoint;

/**
 * One connection's IServiceAttachment over a service provider: an endpoint that answers the control
 * calls and forwards the rest. Releasing closes the connection's subscriptions, and the provider too
 * when the attachment owns it (a provider made for this connection alone).
 */
export class ProviderServiceAttachment : public IServiceAttachment {
public:
    ProviderServiceAttachment(std::shared_ptr<IServiceProvider> provider, std::function<void()> disposeProvider,
                              std::function<void()> onRelease = {});

    Result<std::optional<Json>> invokeService(const Json& call, const IServiceEndpoint::Publisher& publish,
                                              const ServiceContext& context) override;
    void release(const ServiceContext& context) override;

private:
    std::shared_ptr<IServiceProvider> m_provider;
    RemoteServiceEndpoint m_endpoint;
    std::function<void()> m_disposeProvider;
    std::function<void()> m_onRelease;
    std::mutex m_mutex;
    bool m_released = false;
};

ProviderServiceAttachment::ProviderServiceAttachment(std::shared_ptr<IServiceProvider> provider,
                                                     std::function<void()> disposeProvider,
                                                     std::function<void()> onRelease)
    : m_provider(std::move(provider)),
      m_endpoint(*m_provider),
      m_disposeProvider(std::move(disposeProvider)),
      m_onRelease(std::move(onRelease)) {}

Result<std::optional<Json>> ProviderServiceAttachment::invokeService(const Json& call,
                                                                     const IServiceEndpoint::Publisher& publish,
                                                                     const ServiceContext& context) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return std::unexpected(Error{"invalid_state", "Service attachment is released"});
        }
    }
    return m_endpoint.invoke(call, publish, context);
}

void ProviderServiceAttachment::release(const ServiceContext&) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return;
        }
        m_released = true;
    }
    m_endpoint.dispose();
    if (m_disposeProvider) {
        m_disposeProvider();
    }
    if (m_onRelease) {
        m_onRelease();
    }
}
