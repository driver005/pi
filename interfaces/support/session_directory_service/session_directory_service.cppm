export module pi.support.session_directory_service;

import std;
export import pi.chord.i_remote_service;
export import pi.server.i_session_catalog;
export import pi.support.replicated_state;

/**
 * The `pi.session-directory` service: the catalog's sessions as replicated state `{revision,
 * sessions: [{serverId, sessionId, createdAt}]}`. One instance serves every client; refresh()
 * re-reads the catalog and publishes the change. mutations() is the lock that serialises the
 * server-wide create/remove/attach/detach calls. Port of the directory in experimental/services/server.ts.
 */
export class SessionDirectoryService : public IRemoteService {
public:
    SessionDirectoryService(ISessionCatalog& catalog, std::string serverId);

    SessionDirectoryService(const SessionDirectoryService&) = delete;
    SessionDirectoryService& operator=(const SessionDirectoryService&) = delete;

    /** Re-reads the catalog; the state keeps its value when that fails. */
    Result<void> refresh(const ServiceContext& context);
    std::mutex& mutations();
    /** The address of a session as services report it. */
    Json summary(const SessionRecord& record) const;

    std::map<std::string, Method> methods() override;
    std::map<std::string, IReplicatedState*> states() override;

private:
    Result<Json> sessions() const;

    ISessionCatalog& m_catalog;
    std::string m_serverId;
    std::mutex m_mutations;
    std::int64_t m_revision = 1;
    ReplicatedState m_state;
};

SessionDirectoryService::SessionDirectoryService(ISessionCatalog& catalog, std::string serverId)
    : m_catalog(catalog),
      m_serverId(std::move(serverId)),
      m_state(Json{{"revision", 1}, {"sessions", sessions().value_or(Json::array())}}) {}

Json SessionDirectoryService::summary(const SessionRecord& record) const {
    return Json{{"serverId", m_serverId}, {"sessionId", record.id}, {"createdAt", record.createdAt}};
}

Result<Json> SessionDirectoryService::sessions() const {
    auto records = m_catalog.list();
    if (!records) {
        return std::unexpected(records.error());
    }
    Json list = Json::array();
    for (const SessionRecord& record : *records) {
        list.push_back(summary(record));
    }
    return list;
}

Result<void> SessionDirectoryService::refresh(const ServiceContext& context) {
    auto list = sessions();
    if (!list) {
        return std::unexpected(list.error());
    }
    const std::int64_t revision = ++m_revision;
    return m_state.change(context, [&](Json& draft) {
        draft["revision"] = revision;
        draft["sessions"] = *list;
    });
}

std::mutex& SessionDirectoryService::mutations() {
    return m_mutations;
}

std::map<std::string, IRemoteService::Method> SessionDirectoryService::methods() {
    return {};
}

std::map<std::string, IReplicatedState*> SessionDirectoryService::states() {
    return {{"state", &m_state}};
}
