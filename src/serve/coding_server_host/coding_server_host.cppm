export module pi.serve.coding_server_host;

import std;
export import pi.server.i_server_host;
export import pi.server.i_session_catalog;
export import pi.server.i_session_opener;

/**
 * The IServerHost of the coding server: the server-wide services, the session catalog for id
 * resolution, and the opener that brings a cataloged session to life.
 */
export class CodingServerHost : public IServerHost {
public:
    CodingServerHost(IServerServiceHost& services, ISessionCatalog& catalog, ISessionOpener& opener);

    IServerServiceHost& serverServices() override;
    Result<std::string> resolveSession(const std::string& sessionId, const ServiceContext& context) override;
    Result<std::shared_ptr<IRoutedSessionHandle>> openSession(const std::string& sessionId,
                                                              const ServiceContext& context) override;

private:
    IServerServiceHost& m_services;
    ISessionCatalog& m_catalog;
    ISessionOpener& m_opener;
};

CodingServerHost::CodingServerHost(IServerServiceHost& services, ISessionCatalog& catalog, ISessionOpener& opener)
    : m_services(services), m_catalog(catalog), m_opener(opener) {}

IServerServiceHost& CodingServerHost::serverServices() {
    return m_services;
}

Result<std::string> CodingServerHost::resolveSession(const std::string& sessionId, const ServiceContext&) {
    auto record = m_catalog.resolve(sessionId);
    if (!record) {
        return std::unexpected(record.error());
    }
    return record->id;
}

Result<std::shared_ptr<IRoutedSessionHandle>> CodingServerHost::openSession(const std::string& sessionId,
                                                                            const ServiceContext& context) {
    auto record = m_catalog.resolve(sessionId);
    if (!record) {
        return std::unexpected(record.error());
    }
    return m_opener.open(*record, context);
}
