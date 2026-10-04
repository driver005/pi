export module pi.support.remote_service_endpoint;

import std;
export import pi.chord.i_service_endpoint;
export import pi.chord.i_service_provider;
export import pi.support.service_wire;

/**
 * Serves one remote consumer from a provider: answers the `$chord.service` control calls
 * (catalogue, subscribe, unsubscribe), keeps that consumer's subscriptions and forwards every other
 * call to the provider. A subscribe activates the subscription and returns its baseline snapshot;
 * its updates then reach the Publisher given to the same invoke(). Port of
 * createRemoteServiceEndpoint in packages/chord/src/services/provider.ts.
 */
export class RemoteServiceEndpoint : public IServiceEndpoint {
public:
    explicit RemoteServiceEndpoint(IServiceProvider& provider)
        : m_provider(provider) {}

    ~RemoteServiceEndpoint() override {
        dispose();
    }

    Result<std::optional<Json>> invoke(const Json& call, const Publisher& publish, const ServiceContext& context) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_disposed) {
                return std::unexpected(Error{"closed", "Remote service endpoint is disposed"});
            }
        }
        const auto control = m_wire.decodeControl(call);
        if (!control) {
            return m_provider.invoke(call, context);
        }
        if (control->type == "catalogue") {
            return std::optional<Json>(m_provider.catalogue());
        }
        if (control->type == "subscribe") {
            return subscribe(*control, publish);
        }
        return unsubscribe(*control);
    }

    void dispose() override {
        std::map<std::string, ServiceSubscription> subscriptions;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_disposed) {
                return;
            }
            m_disposed = true;
            subscriptions = std::move(m_subscriptions);
            m_subscriptions.clear();
        }
        for (auto& entry : subscriptions) {
            entry.second.close();
        }
    }

private:
    Result<std::optional<Json>> subscribe(const ServiceControlCall& control, const Publisher& publish) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_subscriptions.contains(control.subscriptionId)) {
                return std::unexpected(Error{"invalid_request", "Service subscription ID is already active"});
            }
        }
        const std::string subscriptionId = control.subscriptionId;
        auto subscription = m_provider.subscribe(control.serviceId, control.mode, [publish, subscriptionId](const Json& update, const ServiceContext& context) {
            publish(subscriptionId, update, context);
        });
        if (!subscription) {
            return std::unexpected(subscription.error());
        }
        const Json snapshot = subscription->snapshot;
        std::function<void()> activate = subscription->activate;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_disposed) {
                subscription->close();
                return std::unexpected(Error{"closed", "Remote service endpoint is disposed"});
            }
            m_subscriptions.emplace(subscriptionId, std::move(*subscription));
        }
        activate();
        return std::optional<Json>(snapshot);
    }

    Result<std::optional<Json>> unsubscribe(const ServiceControlCall& control) {
        ServiceSubscription subscription;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_subscriptions.find(control.subscriptionId);
            if (found == m_subscriptions.end()) {
                return std::unexpected(Error{"invalid_request", "Service subscription was not found"});
            }
            subscription = std::move(found->second);
            m_subscriptions.erase(found);
        }
        subscription.close();
        return std::optional<Json>();
    }

    IServiceProvider& m_provider;
    ServiceWire m_wire;
    std::mutex m_mutex;
    std::map<std::string, ServiceSubscription> m_subscriptions;
    bool m_disposed = false;
};
