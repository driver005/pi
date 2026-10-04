export module pi.testing.scripted_mcp_connector;

import std;
export import pi.mcp.i_mcp_connector;
import pi.mcp.mcp_client;
export import pi.testing.scripted_mcp_transport;

/**
 * IMcpConnector that hands every connect call the next queued script: an in-memory server set up
 * by the callback, connected through a real McpClient, or a queued failure. Records the
 * transports and the options of every attempt.
 */
export class ScriptedMcpConnector : public IMcpConnector {
public:
    using Setup = std::function<void(ScriptedMcpTransport&)>;

    void enqueue(Setup setup) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_script.emplace_back(std::move(setup));
    }

    void enqueueFailure(Error error) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_script.emplace_back(std::unexpected(std::move(error)));
    }

    Result<std::unique_ptr<IMcpClient>> connect(const McpServerConfig&, const std::string& cwd, const McpClientOptions& options) override {
        Result<Setup> next = std::unexpected(Error{"script", "no connection scripted"});
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_attempts;
            m_options.push_back(options);
            m_directories.push_back(cwd);
            if (!m_script.empty()) {
                next = std::move(m_script.front());
                m_script.pop_front();
            }
        }
        if (!next) {
            return std::unexpected(next.error());
        }
        auto transport = std::make_unique<ScriptedMcpTransport>();
        (*next)(*transport);
        ScriptedMcpTransport* raw = transport.get();
        auto client = std::make_unique<McpClient>(options);
        auto connected = client->connect(std::move(transport));
        if (!connected) {
            return std::unexpected(connected.error());
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_transports.push_back(raw);
        }
        return std::unique_ptr<IMcpClient>(std::move(client));
    }

    int attempts() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_attempts;
    }

    /** Transports of the successful attempts, valid while their clients live. */
    std::vector<ScriptedMcpTransport*> transports() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_transports;
    }

    std::vector<McpClientOptions> options() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_options;
    }

    std::vector<std::string> directories() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_directories;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<Result<Setup>> m_script;
    std::vector<ScriptedMcpTransport*> m_transports;
    std::vector<McpClientOptions> m_options;
    std::vector<std::string> m_directories;
    int m_attempts = 0;
};
