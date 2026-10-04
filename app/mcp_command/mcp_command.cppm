export module pi.mcp_command;

import std;
export import pi.coding_services;
export import pi.platform.i_http_client;
export import pi.support.mcp_oauth_sign_in;
export import pi.types.command_line;
import pi.session.settings_manager;
import pi.support.config_value_resolver;
import pi.support.mcp_config_loader;

/**
 * `pi mcp list | login <server> | logout <server>`: the sign-in of remote MCP servers that authenticate with OAuth. `login`
 * runs the authorization code flow (McpOauthSignIn): it prints the authorization URL, waits for the browser at the loopback
 * callback and stores the credentials in `<agent-dir>/mcp-auth.json`, which sessions read; `logout` forgets them; `list` shows
 * the configured servers and how each authenticates. Servers come from the global and (when trusted) project mcp.json. Port of
 * the login and logout commands of extensions/mcp/cli.ts. Returns the process exit code.
 */
export class McpCommand {
public:
    McpCommand(CodingServices& services, IHttpClient& http, McpOauthSignIn::CallbackFactory callbacks, std::ostream& out, std::ostream& err)
        : m_services(services),
          m_http(http),
          m_callbacks(std::move(callbacks)),
          m_out(out),
          m_err(err) {}

    int run(const CommandLine& line) {
        const std::string& agentDir = line.options.agentDir;
        PlatformServices& platform = m_services.platform();
        SettingsManager settings(agentDir + "/settings.json", line.options.cwd + "/.pi/settings.json", false, platform.files(), platform.locks(), platform.ids());
        const auto trusted = m_services.trust().resolve(line.options.cwd, line.options.startup.trustProject, settings.view().defaultProjectTrust(), {});
        McpConfigLoadOptions load;
        load.agentDir = agentDir;
        load.cwd = line.options.cwd;
        load.projectTrusted = trusted && *trusted;
        const McpConfigResult config = McpConfigLoader(platform.files()).load(load);
        for (const std::string& problem : config.errors) {
            m_err << "pi: MCP config: " << problem << "\n";
        }
        McpOauthStore store(agentDir + "/mcp-auth.json", agentDir, platform.files(), platform.locks(), platform.crypto());
        const std::string& subcommand = line.arguments[0];
        if (subcommand == "list") {
            return list(config, store);
        }
        const auto server = find(config, line.arguments[1]);
        if (!server) {
            return 1;
        }
        return subcommand == "login" ? login(*server, store) : logout(*server, store);
    }

private:
    int list(const McpConfigResult& config, McpOauthStore& store) {
        for (const McpServerConfig& server : config.servers) {
            std::string how = "stdio";
            if (server.http) {
                how = usesOauth(server) ? oauthState(server, store) : server.authProvider ? "provider " + *server.authProvider : "no OAuth";
            }
            m_out << server.name << "\t" << (server.http ? server.url : server.command) << "\t" << how << (server.enabled ? "" : "\t(disabled)") << "\n";
        }
        return 0;
    }

    std::string oauthState(const McpServerConfig& server, McpOauthStore& store) {
        const auto state = store.load(server.name, server.url);
        const bool signedIn = state && state->has_value() && (**state).contains("tokens");
        return signedIn ? "OAuth: signed in" : "OAuth: not signed in";
    }

    int login(const McpServerConfig& server, McpOauthStore& store) {
        PlatformServices& platform = m_services.platform();
        auto settings = oauthSettings(server);
        if (!settings) {
            m_err << "pi: " << settings.error().message << "\n";
            return 1;
        }
        McpOauthRefresher refresher(m_http, platform.clock(), platform.base64());
        McpOauthSignIn signIn(store, refresher, m_http, platform.crypto(), platform.base64(), m_callbacks);
        const auto done = signIn.signIn(server.name, server.url, *settings, Json(), [&](const std::string& url) {
            m_out << "Open this URL in a browser to sign in to \"" << server.name << "\":\n\n  " << url << "\n\nWaiting for the browser...\n";
            m_out.flush();
        }, McpOauthSignIn::kDefaultTimeout, nullptr);
        if (!done) {
            m_err << "pi: sign-in to \"" << server.name << "\" failed: " << done.error().message << "\n";
            return 1;
        }
        m_out << "Signed in to \"" << server.name << "\".\n";
        return 0;
    }

    int logout(const McpServerConfig& server, McpOauthStore& store) {
        const auto removed = store.remove(server.name, server.url);
        if (!removed) {
            m_err << "pi: " << removed.error().message << "\n";
            return 1;
        }
        m_out << (*removed ? "Signed out of \"" + server.name + "\".\n" : "No credentials are stored for \"" + server.name + "\".\n");
        return 0;
    }

    /** The server to sign in to: it must be an HTTP server that authenticates with OAuth. */
    std::optional<McpServerConfig> find(const McpConfigResult& config, const std::string& name) {
        for (const McpServerConfig& server : config.servers) {
            if (server.name != name) {
                continue;
            }
            if (!server.http) {
                m_err << "pi: MCP server \"" << name << "\" is a stdio server; only remote servers sign in\n";
                return std::nullopt;
            }
            if (!usesOauth(server)) {
                m_err << "pi: MCP server \"" << name << "\" does not use OAuth (it has an Authorization header or auth.provider)\n";
                return std::nullopt;
            }
            return server;
        }
        m_err << "pi: no MCP server named \"" << name << "\" is configured\n";
        return std::nullopt;
    }

    bool usesOauth(const McpServerConfig& server) const {
        if (!server.http || server.authProvider) {
            return false;
        }
        return std::none_of(server.headers.begin(), server.headers.end(), [](const auto& header) {
            std::string name = header.first;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name == "authorization";
        });
    }

    /** The server's `oauth` settings with the client secret resolved. */
    Result<Json> oauthSettings(const McpServerConfig& server) {
        Json settings = server.raw.is_object() && server.raw.contains("oauth") && server.raw["oauth"].is_object() ? server.raw["oauth"] : Json::object();
        if (settings.contains("clientSecret") && settings["clientSecret"].is_string()) {
            ConfigValueResolver resolver(m_services.platform().environment(), m_services.platform().processes());
            auto secret = resolver.resolveOrError(settings["clientSecret"].get<std::string>(), "MCP server \"" + server.name + "\" oauth.clientSecret", {});
            if (!secret) {
                return std::unexpected(secret.error());
            }
            settings["clientSecret"] = *secret;
        }
        return settings;
    }

    CodingServices& m_services;
    IHttpClient& m_http;
    McpOauthSignIn::CallbackFactory m_callbacks;
    std::ostream& m_out;
    std::ostream& m_err;
};
