export module pi.coding_runtime_factory;

import std;
export import pi.coding_services;
export import pi.session.i_session_runtime_factory;
export import pi.types.coding_startup_options;
import pi.coding_session_handle;

/** ISessionRuntimeFactory that builds a CodingSessionHandle for each session the runtime starts. */
export class CodingRuntimeFactory : public ISessionRuntimeFactory {
public:
    CodingRuntimeFactory(CodingServices& services, CodingStartupOptions options);

    Result<std::unique_ptr<ISessionRuntimeHandle>> create(SessionRuntimeRequest request) override;

private:
    CodingServices& m_services;
    CodingStartupOptions m_options;
};

CodingRuntimeFactory::CodingRuntimeFactory(CodingServices& services, CodingStartupOptions options)
    : m_services(services), m_options(std::move(options)) {}

Result<std::unique_ptr<ISessionRuntimeHandle>> CodingRuntimeFactory::create(SessionRuntimeRequest request) {
    if (!request.sessionManager) {
        return std::unexpected(Error{"invalid_request", "A session runtime needs a session manager"});
    }
    if (!m_services.platform().files().exists(request.cwd)) {
        return std::unexpected(Error{"missing_cwd", "Working directory does not exist: " + request.cwd});
    }
    return std::unique_ptr<ISessionRuntimeHandle>(
        std::make_unique<CodingSessionHandle>(std::move(request), m_services, m_options));
}
