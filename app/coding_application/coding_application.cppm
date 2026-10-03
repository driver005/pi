export module pi.coding_application;

import std;
export import pi.platform.i_byte_input;
export import pi.platform.i_byte_output;
export import pi.session.i_agent_session_runtime;
export import pi.types.coding_application_options;
export import pi.types.result;
import pi.coding_runtime_factory;
import pi.coding_services;
import pi.rpc.rpc_server;
import pi.session.agent_session_runtime;

/**
 * The headless coding application: the shared services, the session factory and the runtime that
 * owns the current session. open() starts the first session as the options ask; run modes then
 * serve it. Diagnostics from setting up the session are available after open().
 */
export class CodingApplication {
public:
    explicit CodingApplication(CodingApplicationOptions options)
        : m_options(std::move(options)),
          m_services(m_options.agentDir, m_options.catalogDir.empty() ? m_options.agentDir + "/catalog" : m_options.catalogDir, m_options.faux),
          m_factory(m_services, m_options.startup) {}

    Result<void> open() {
        auto manager = openSessionTree();
        if (!manager) {
            return std::unexpected(manager.error());
        }
        SessionRuntimeRequest request;
        request.cwd = m_options.cwd;
        request.agentDir = m_options.agentDir;
        request.sessionManager = std::move(*manager);
        auto handle = m_factory.create(std::move(request));
        if (!handle) {
            return std::unexpected(handle.error());
        }
        m_runtime = std::make_unique<AgentSessionRuntime>(m_factory, m_services.sessions(), m_services.platform().files(),
                                                          std::move(*handle));
        return {};
    }

    /** Serves JSONL RPC until the input ends; returns the process exit code. */
    int runRpc(IByteInput& input, IByteOutput& output) {
        if (!m_runtime) {
            return 1;
        }
        RpcServer server(*m_runtime, m_services.models().models(), input, output, m_services.platform().executor());
        return server.run();
    }

    std::vector<RuntimeDiagnostic> diagnostics() const {
        return m_runtime ? m_runtime->diagnostics() : std::vector<RuntimeDiagnostic>{};
    }

    IAgentSessionRuntime* runtime() {
        return m_runtime.get();
    }

    CodingServices& services() {
        return m_services;
    }

private:
    Result<std::unique_ptr<ISessionManager>> openSessionTree() {
        ISessionStore& sessions = m_services.sessions();
        switch (m_options.sessionMode) {
        case SessionStartMode::Continue:
            return sessions.continueRecent(m_options.cwd, m_options.sessionDir);
        case SessionStartMode::InMemory:
            return sessions.inMemory(m_options.cwd);
        case SessionStartMode::Open:
            return openReference(m_options.sessionRef.value_or(""));
        case SessionStartMode::New:
            break;
        }
        return sessions.create(m_options.cwd, m_options.sessionDir, std::nullopt, std::nullopt);
    }

    Result<std::unique_ptr<ISessionManager>> openReference(const std::string& reference) {
        if (isPath(reference)) {
            return m_services.sessions().open(reference, m_options.sessionDir, std::nullopt);
        }
        const auto path = m_services.sessions().findById(m_options.cwd, reference, m_options.sessionDir);
        if (!path) {
            return std::unexpected(Error{"session_not_found", "No session with id " + reference});
        }
        return m_services.sessions().open(*path, m_options.sessionDir, std::nullopt);
    }

    bool isPath(const std::string& reference) const {
        return reference.find('/') != std::string::npos || reference.ends_with(".jsonl");
    }

    CodingApplicationOptions m_options;
    CodingServices m_services;
    CodingRuntimeFactory m_factory;
    std::unique_ptr<AgentSessionRuntime> m_runtime;
};
