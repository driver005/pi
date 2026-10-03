export module pi.serve.served_session_opener;

import std;
export import pi.platform.i_executor;
export import pi.platform.i_id_generator;
export import pi.provider.i_model_runtime;
export import pi.server.i_session_opener;
export import pi.session.i_session_runtime_factory;
export import pi.session.i_session_store;
import pi.support.served_session;

/**
 * ISessionOpener that starts the coding session of a cataloged session: it opens the session tree in
 * the session's directory (the newest session file there, or a new one named after the catalog id)
 * and serves it through a ServedSession.
 */
export class ServedSessionOpener : public ISessionOpener {
public:
    ServedSessionOpener(ISessionStore& store, ISessionRuntimeFactory& runtimes, IModelRuntime& models, IExecutor& executor, IIdGenerator& ids, std::string agentDir)
        : m_store(store),
          m_runtimes(runtimes),
          m_models(models),
          m_executor(executor),
          m_ids(ids),
          m_agentDir(std::move(agentDir)) {}

    Result<std::shared_ptr<IRoutedSessionHandle>> open(const SessionRecord& record, const ServiceContext&) override {
        auto tree = openTree(record);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        SessionRuntimeRequest request;
        request.cwd = record.cwd;
        request.agentDir = m_agentDir;
        request.sessionManager = std::move(*tree);
        auto runtime = m_runtimes.create(std::move(request));
        if (!runtime) {
            return std::unexpected(runtime.error());
        }
        return std::shared_ptr<IRoutedSessionHandle>(
            std::make_shared<ServedSession>(std::move(*runtime), m_models, m_executor, m_ids));
    }

private:
    Result<std::unique_ptr<ISessionManager>> openTree(const SessionRecord& record) {
        if (m_store.findMostRecent(record.directory, record.cwd)) {
            return m_store.continueRecent(record.cwd, record.directory);
        }
        return m_store.create(record.cwd, record.directory, record.id, std::nullopt);
    }

    ISessionStore& m_store;
    ISessionRuntimeFactory& m_runtimes;
    IModelRuntime& m_models;
    IExecutor& m_executor;
    IIdGenerator& m_ids;
    std::string m_agentDir;
};
