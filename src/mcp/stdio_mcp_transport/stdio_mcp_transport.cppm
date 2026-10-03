export module pi.mcp.stdio_mcp_transport;

import std;
export import pi.mcp.i_mcp_transport;
export import pi.platform.i_child_process_launcher;
export import pi.support.mcp_message_codec;
export import pi.types.mcp_stdio_options;

/**
 * MCP over a child process: one JSON-RPC message per line on stdin and stdout, stderr kept as a
 * bounded tail for diagnostics. Closing follows the specification: close stdin and let the server
 * exit, then SIGTERM, then SIGKILL. Port of packages/mcp/src/transports/stdio.ts.
 */
export class StdioMcpTransport : public IMcpTransport {
public:
    StdioMcpTransport(IChildProcessLauncher& launcher, McpStdioOptions options);
    ~StdioMcpTransport() override;

    Result<void> start() override;
    Result<void> send(const Json& message) override;
    void close() override;
    void setMessageListener(MessageListener listener) override;
    void setErrorListener(ErrorListener listener) override;
    void setCloseListener(CloseListener listener) override;
    void setProtocolVersion(const std::string& version) override;

    /** The last bytes the server wrote to stderr (at most maxStderrBytes). */
    std::string stderrTail() const;

private:
    void readOutput();
    void readErrors();
    void handleChunk(const std::string& chunk, std::string& buffer);
    void handleLine(const std::string& line);
    void emitError(const Error& error);
    void emitClose();

    IChildProcessLauncher& m_launcher;
    McpStdioOptions m_options;
    McpMessageCodec m_codec;
    mutable std::mutex m_mutex;
    std::unique_ptr<IChildProcess> m_child;
    MessageListener m_message;
    ErrorListener m_error;
    CloseListener m_close;
    std::string m_stderr;
    bool m_started = false;
    bool m_closing = false;
    bool m_closeEmitted = false;
    std::thread m_stdoutReader;
    std::thread m_stderrReader;
};

StdioMcpTransport::StdioMcpTransport(IChildProcessLauncher& launcher, McpStdioOptions options)
    : m_launcher(launcher), m_options(std::move(options)) {}

StdioMcpTransport::~StdioMcpTransport() {
    close();
}

void StdioMcpTransport::setMessageListener(MessageListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_message = std::move(listener);
}

void StdioMcpTransport::setErrorListener(ErrorListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_error = std::move(listener);
}

void StdioMcpTransport::setCloseListener(CloseListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_close = std::move(listener);
}

void StdioMcpTransport::setProtocolVersion(const std::string&) {}

std::string StdioMcpTransport::stderrTail() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stderr;
}

void StdioMcpTransport::emitError(const Error& error) {
    ErrorListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_error;
    }
    if (listener) {
        listener(error);
    }
}

void StdioMcpTransport::emitClose() {
    CloseListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closeEmitted) {
            return;
        }
        m_closeEmitted = true;
        listener = m_close;
    }
    if (listener) {
        listener();
    }
}

Result<void> StdioMcpTransport::start() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started) {
            return std::unexpected(Error{"closed", "MCP stdio transport already started"});
        }
        if (m_closing) {
            return std::unexpected(Error{"closed", "MCP connection closed"});
        }
        m_started = true;
    }
    ProcessRequest request;
    request.command = m_options.command;
    request.args = m_options.args;
    request.cwd = m_options.cwd;
    request.env = m_options.env;
    auto child = m_launcher.launch(request);
    if (!child) {
        return std::unexpected(child.error());
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_child = std::move(*child);
    }
    m_stdoutReader = std::thread([this]() { readOutput(); });
    m_stderrReader = std::thread([this]() { readErrors(); });
    return {};
}

void StdioMcpTransport::handleLine(const std::string& rawLine) {
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
        return;
    }
    if (line.size() > m_options.maxMessageBytes) {
        emitError(Error{"protocol", "MCP stdio message exceeds " + std::to_string(m_options.maxMessageBytes) + " bytes"});
        return;
    }
    const Json message = Json::parse(line, nullptr, false);
    if (!(m_codec.isRequest(message) || m_codec.isNotification(message) || m_codec.isResponse(message))) {
        emitError(Error{"protocol", "Invalid JSON-RPC message: " + line.substr(0, 200)});
        return;
    }
    MessageListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_message;
    }
    if (listener) {
        listener(message);
    }
}

void StdioMcpTransport::handleChunk(const std::string& chunk, std::string& buffer) {
    buffer += chunk;
    std::size_t newline = buffer.find('\n');
    while (newline != std::string::npos) {
        handleLine(buffer.substr(0, newline));
        buffer.erase(0, newline + 1);
        newline = buffer.find('\n');
    }
    if (buffer.size() > m_options.maxMessageBytes) {
        buffer.clear();
        emitError(Error{"protocol", "MCP stdio message exceeds " + std::to_string(m_options.maxMessageBytes) + " bytes"});
    }
}

void StdioMcpTransport::readOutput() {
    std::string buffer;
    while (auto chunk = m_child->readOutput()) {
        handleChunk(*chunk, buffer);
    }
    if (buffer.find_first_not_of(" \t\r\n") != std::string::npos) {
        emitError(Error{"protocol", "MCP stdio server closed with an incomplete JSON-RPC message"});
    }
    emitClose();
}

void StdioMcpTransport::readErrors() {
    while (auto chunk = m_child->readError()) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stderr += *chunk;
        if (m_stderr.size() > m_options.maxStderrBytes) {
            m_stderr.erase(0, m_stderr.size() - m_options.maxStderrBytes);
        }
    }
}

Result<void> StdioMcpTransport::send(const Json& message) {
    IChildProcess* child = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_closing || !m_child) {
            return std::unexpected(Error{"closed", "MCP connection closed"});
        }
        child = m_child.get();
    }
    return child->write(message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n");
}

void StdioMcpTransport::close() {
    IChildProcess* child = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closing) {
            return;
        }
        m_closing = true;
        child = m_child.get();
    }
    if (child == nullptr) {
        emitClose();
        return;
    }
    // Shutdown per the specification: close stdin and let the server exit, then SIGTERM, then SIGKILL.
    child->closeStdin();
    const auto grace = std::chrono::milliseconds(std::min<std::int64_t>(500, m_options.closeTimeoutMs));
    if (!child->waitForExit(grace)) {
        child->terminate();
        if (!child->waitForExit(std::chrono::milliseconds(m_options.closeTimeoutMs))) {
            child->kill();
            child->waitForExit(std::chrono::seconds(5));
        }
    }
    // The server's own children may have outlived it; end the group.
    child->terminate();
    if (m_stdoutReader.joinable()) {
        m_stdoutReader.join();
    }
    if (m_stderrReader.joinable()) {
        m_stderrReader.join();
    }
    emitClose();
}
