export module pi.session_manager_factory;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.session.i_session_manager_factory;
import pi.session.session_manager;

/** ISessionManagerFactory creating opened SessionManager instances. */
export class SessionManagerFactory : public ISessionManagerFactory {
public:
    SessionManagerFactory(IFileSystem& files, const IClock& clock, IIdGenerator& ids);

    Result<std::unique_ptr<ISessionManager>> create(const SessionManagerOptions& options) override;

private:
    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
};

SessionManagerFactory::SessionManagerFactory(IFileSystem& files, const IClock& clock, IIdGenerator& ids)
    : m_files(files), m_clock(clock), m_ids(ids) {}

Result<std::unique_ptr<ISessionManager>> SessionManagerFactory::create(const SessionManagerOptions& options) {
    auto manager = std::make_unique<SessionManager>(options, m_files, m_clock, m_ids);
    if (auto opened = manager->open(); !opened) {
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<ISessionManager>(std::move(manager));
}
