export module pi.mcp_connector;

import std;
export import pi.mcp.i_mcp_connector;
export import pi.platform.i_child_process_launcher;
export import pi.platform.i_file_system;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.support.config_value_resolver;
export import pi.support.mcp_oauth_providers;
export import pi.support.path_resolver;
export import pi.types.mcp_http_options;
export import pi.types.mcp_stdio_options;
import pi.mcp.mcp_client;
import pi.mcp.stdio_mcp_transport;
import pi.mcp.streamable_http_mcp_transport;

/**
 * Connects configured MCP servers: stdio servers as child processes, HTTP servers over the
 * streamable HTTP transport. `${VAR}` and `!cmd` values in env and headers are resolved here, `~`
 * and relative paths too. Port of createDefaultTransport and connectOnce in
 * packages/coding-agent/src/extensions/mcp/runtime.ts. HTTP servers without an `Authorization` header or `auth.provider`
 * send the OAuth token stored by a sign-in (see McpOauthTokenProvider).
 */
export class McpConnector : public IMcpConnector {
public:
    /** Returns the current token of a pi provider (servers with `auth.provider`), or nullopt. */
    using ProviderToken = std::function<std::optional<std::string>(const std::string&)>;

    /** `oauth` (optional) supplies the tokens of HTTP servers that authenticate with OAuth. */
    McpConnector(IChildProcessLauncher& launcher, IHttpClient& http, ISleeper& sleeper, IFileSystem& files, ConfigValueResolver& resolver, ProviderToken providerToken, McpOauthProviders* oauth = nullptr)
        : m_launcher(launcher),
          m_http(http),
          m_sleeper(sleeper),
          m_files(files),
          m_resolver(resolver),
          m_providerToken(std::move(providerToken)),
          m_oauth(oauth) {}

    Result<std::unique_ptr<IMcpClient>> connect(const McpServerConfig& config, const std::string& cwd, const McpClientOptions& options) override {
        auto transport = config.http ? httpTransport(config) : stdioTransport(config, cwd);
        if (!transport) {
            return std::unexpected(transport.error());
        }
        StdioMcpTransport* stdio = config.http ? nullptr : static_cast<StdioMcpTransport*>(transport->get());
        auto client = std::make_unique<McpClient>(options);
        auto connected = client->connect(std::move(*transport));
        if (!connected) {
            std::string message = connected.error().message;
            const std::string stderrTail = stdio != nullptr ? tail(stdio->stderrTail()) : "";
            client->close();
            return std::unexpected(Error{connected.error().code,
                                         stderrTail.empty() ? message : message + "\n" + stderrTail});
        }
        return std::unique_ptr<IMcpClient>(std::move(client));
    }

private:
    Result<std::unique_ptr<IMcpTransport>> stdioTransport(const McpServerConfig& config, const std::string& cwd) {
        McpStdioOptions stdio;
        stdio.command = expandHome(config.command);
        for (const std::string& arg : config.args) {
            stdio.args.push_back(expandHome(arg));
        }
        for (const auto& [key, value] : config.env) {
            auto resolved = m_resolver.resolveOrError(value, describe(config) + " env \"" + key + "\"", {});
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            stdio.env[key] = *resolved;
        }
        stdio.cwd = PathResolver(m_files.homeDirectory()).resolveToCwd(expandHome(config.cwd.value_or(".")), cwd);
        return std::unique_ptr<IMcpTransport>(std::make_unique<StdioMcpTransport>(m_launcher, std::move(stdio)));
    }

    Result<std::unique_ptr<IMcpTransport>> httpTransport(const McpServerConfig& config) {
        McpHttpOptions http;
        http.url = config.url;
        const auto headers = m_resolver.resolveHeadersOrError(config.headers, describe(config), {});
        if (!headers) {
            return std::unexpected(headers.error());
        }
        for (const auto& [name, value] : *headers) {
            http.headers.emplace_back(name, value);
        }
        if (config.authProvider) {
            // Read on every request, so the provider's token refreshes apply.
            const std::string provider = *config.authProvider;
            http.bearerToken = [this, provider]() { return m_providerToken(provider).value_or(""); };
        }
        if (m_oauth != nullptr && m_oauth->usesOauth(config)) {
            // The stored token is read before every request and refreshed when it expires or the server rejects it.
            const std::shared_ptr<McpOauthTokenProvider> provider = m_oauth->providerFor(config);
            http.bearerToken = [provider]() { return provider->token(); };
            http.onUnauthorized = [provider](const std::string& challenge, const std::string& stale) { return provider->onUnauthorized(challenge, stale); };
        }
        return std::unique_ptr<IMcpTransport>(
            std::make_unique<StreamableHttpMcpTransport>(m_http, m_sleeper, std::move(http)));
    }

    std::string expandHome(const std::string& value) const {
        const std::string home = m_files.homeDirectory();
        if (value == "~") {
            return home;
        }
        if (value.starts_with("~/")) {
            return home + value.substr(1);
        }
        return value;
    }

    std::string describe(const McpServerConfig& config) const {
        return "MCP server \"" + config.name + "\"";
    }

    std::string tail(const std::string& text) const {
        constexpr std::size_t maxChars = 2000;
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        std::string trimmed = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        return trimmed.size() > maxChars ? trimmed.substr(trimmed.size() - maxChars) : trimmed;
    }

    IChildProcessLauncher& m_launcher;
    IHttpClient& m_http;
    ISleeper& m_sleeper;
    IFileSystem& m_files;
    ConfigValueResolver& m_resolver;
    ProviderToken m_providerToken;
    McpOauthProviders* m_oauth;
};
