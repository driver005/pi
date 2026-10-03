export module pi.serve.server_service_host;

import std;
export import pi.server.i_server_service_host;
export import pi.server.i_session_catalog;
import pi.support.provider_service_attachment;
import pi.support.remote_service_provider;
import pi.support.session_directory_service;
import pi.support.session_management_service;

/**
 * The server-wide services (`pi.session-directory`, `pi.session-management`) for every client: the
 * directory is shared, each client gets its own management service bound to its presentation.
 * Port of createExperimentalServerServices in experimental/services/server.ts.
 */
export class ServerServiceHost : public IServerServiceHost {
public:
    ServerServiceHost(ISessionCatalog& catalog, std::string serverId)
        : m_catalog(catalog),
          m_directory(std::make_shared<SessionDirectoryService>(catalog, std::move(serverId))) {}

    Result<std::unique_ptr<IServiceAttachment>> attachClient(IServerPresentation& presentation, const ServiceContext&) override {
        auto provider = std::make_shared<RemoteServiceProvider>(std::vector<ServiceDefinition>{
            {"pi.session-directory", "singleton"}, {"pi.session-management", "singleton"}});
        if (auto provided = provider->provide("pi.session-directory", m_directory); !provided) {
            return std::unexpected(provided.error());
        }
        auto management = std::make_shared<SessionManagementService>(presentation, m_catalog, *m_directory);
        if (auto provided = provider->provide("pi.session-management", management); !provided) {
            return std::unexpected(provided.error());
        }
        return std::unique_ptr<IServiceAttachment>(
            std::make_unique<ProviderServiceAttachment>(provider, [provider] { provider->dispose(); }));
    }

    /** Re-reads the catalog and publishes the new directory to every client. */
    Result<void> refresh(const ServiceContext& context) {
        return m_directory->refresh(context);
    }

private:
    ISessionCatalog& m_catalog;
    std::shared_ptr<SessionDirectoryService> m_directory;
};
