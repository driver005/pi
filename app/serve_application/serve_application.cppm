export module pi.serve_application;

import std;
export import pi.types.command_line;
export import pi.types.result;
export import pi.types.serve_dependencies;
import pi.base.posix_byte_connection;
import pi.base.posix_unix_listener;
import pi.base.thread_pool;
import pi.serve.coding_server_host;
import pi.serve.directory_session_catalog;
import pi.serve.served_session_opener;
import pi.serve.server_service_host;
import pi.server.server;
import pi.support.server_identity;

/**
 * The headless protocol server: `pi serve`. Wires the coding services, a session catalog on disk, the
 * server-wide services, the session opener and a unix-socket listener into a Server. Requests run on
 * their own thread pool, separate from the one that runs agents, so waiting calls can never starve
 * the work they wait for.
 */
export class ServeApplication {
public:
    ServeApplication(CommandLine line, const ServeDependencies& dependencies)
        : m_line(std::move(line)),
          m_deps(dependencies),
          m_requests(64),
          m_work(32) {}

    ~ServeApplication() {
        stop();
    }

    ServeApplication(const ServeApplication&) = delete;
    ServeApplication& operator=(const ServeApplication&) = delete;

    /** Resolves the identity, prepares the directories and starts listening. */
    Result<void> start() {
        ServerIdentity identity(*m_deps.files, *m_deps.crypto);
        auto resolved = identity.resolve(m_line.serverDir, m_line.serverId);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        m_serverId = *resolved;
        if (auto made = m_deps.files->createPrivateDirectories(m_line.serverDir); !made) {
            return std::unexpected(made.error());
        }
        m_catalog = std::make_unique<DirectorySessionCatalog>(*m_deps.files, *m_deps.clock, *m_deps.ids,
                                                              m_line.options.sessionDir.value_or(""), m_line.options.cwd);
        m_serverServices = std::make_unique<ServerServiceHost>(*m_catalog, m_serverId);
        m_opener = std::make_unique<ServedSessionOpener>(*m_deps.sessions, *m_deps.runtimes, *m_deps.models, m_work,
                                                         *m_deps.ids, m_line.options.agentDir);
        m_host = std::make_unique<CodingServerHost>(*m_serverServices, *m_catalog, *m_opener);
        UnixListenerOptions listenerOptions;
        listenerOptions.path = socketPath();
        listenerOptions.onError = [this](const Error& error) { report(error); };
        listenerOptions.connect = [](int fd, std::uint64_t limit, std::int64_t grace) {
            return std::shared_ptr<ISocketConnection>(std::make_shared<PosixByteConnection>(fd, limit, grace));
        };
        m_listener = std::make_unique<PosixUnixListener>(listenerOptions);
        ServerOptions serverOptions;
        serverOptions.serverId = m_serverId;
        serverOptions.onError = [this](const Error& error) { report(error); };
        m_server = std::make_unique<Server>(*m_host, m_requests, *m_deps.ids,
                                            std::vector<IServerListener*>{m_listener.get()}, serverOptions);
        return m_server->start();
    }

    /** Disconnects every client, closes every session and stops listening. Idempotent. */
    void stop() {
        if (m_server) {
            if (auto closed = m_server->close(); !closed) {
                report(closed.error());
            }
        }
    }

    const std::string& serverId() const {
        return m_serverId;
    }

    std::string socketPath() const {
        return m_line.serverDir + "/" + m_serverId + ".sock";
    }

private:
    void report(const Error& error) {
        m_deps.logger->log(LogLevel::Warn, "server: " + error.code + ": " + error.message);
    }

    CommandLine m_line;
    ServeDependencies m_deps;
    ThreadPool m_requests;
    ThreadPool m_work;
    std::string m_serverId;
    std::unique_ptr<DirectorySessionCatalog> m_catalog;
    std::unique_ptr<ServerServiceHost> m_serverServices;
    std::unique_ptr<ServedSessionOpener> m_opener;
    std::unique_ptr<CodingServerHost> m_host;
    std::unique_ptr<PosixUnixListener> m_listener;
    std::unique_ptr<Server> m_server;
};
