export module pi.auth_command;

import std;
export import pi.coding_services;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
import pi.plugin.plugin_host;
import pi.support.hook_bus;
import pi.support.plugin_discovery;
import pi.tools.tool_registry;
export import pi.support.provider_login;
export import pi.types.command_line;

/**
 * `pi auth list | status | login <provider> | logout <provider>`: sign-in to the subscription providers (Anthropic, OpenAI
 * Codex and ChatGPT, OpenRouter, xAI, Kimi Code, GitHub Copilot). `login` runs the provider's flow (ProviderLogin): it prints the
 * authorization URL or the device code and waits for the browser at the loopback callback (`--manual` asks for the pasted code
 * instead, which is also the fallback when a port the flow shares with other tools is taken), then stores the credential in
 * `<agent-dir>/auth.json`, which sessions refresh as it expires; `--method` picks one of the provider's methods. `logout` forgets
 * the credential, `status` lists the stored ones and `list` the providers that can sign in. Plugins (<agent-dir>/plugins and
 * `--plugin`) are loaded for `list` and `login`, so providers whose plugin registered an OAuth sign-in (register_oauth) appear and
 * sign in with method "plugin". Returns the process exit code.
 */
export class AuthCommand {
public:
    AuthCommand(CodingServices& services, IHttpClient& http, ISleeper& sleeper, OauthBrowserLogin::CallbackFactory callbacks, std::istream& in, std::ostream& out, std::ostream& err)
        : m_services(services),
          m_http(http),
          m_sleeper(sleeper),
          m_callbacks(std::move(callbacks)),
          m_in(in),
          m_out(out),
          m_err(err) {}

    int run(const CommandLine& line) {
        ModelServices& models = m_services.models();
        PlatformServices& platform = m_services.platform();
        ProviderLogin login(models.credentials(), m_http, platform.crypto(), platform.base64(), platform.clock(), m_sleeper, m_callbacks, models.flows(), models.kimiHost());
        const std::string& subcommand = line.arguments[0];
        ToolRegistry tools;
        HookBus hooks;
        PluginHost plugins(platform.libraries(), tools, hooks, platform.processes(), platform.logger(), PluginContext{line.options.cwd, line.options.agentDir}, &models.models(), nullptr, &platform.clock());
        if ((subcommand == "list" || subcommand == "login") && !line.options.startup.noPlugins) {
            loadPlugins(plugins, line);
            login.setPlugins(&plugins);
        }
        if (subcommand == "list") {
            return list(login);
        }
        if (subcommand == "status") {
            return status(models.credentials(), platform.clock());
        }
        if (subcommand == "logout") {
            return logout(login, line.arguments[1]);
        }
        return signIn(login, line);
    }

private:
    /** The plugins of <agent-dir>/plugins and --plugin; problems are warnings. */
    void loadPlugins(PluginHost& plugins, const CommandLine& line) {
        PluginDiscovery discovery(m_services.platform().files());
        std::vector<std::string> paths = discovery.discover(line.options.agentDir + "/plugins");
        paths.insert(paths.end(), line.options.startup.pluginPaths.begin(), line.options.startup.pluginPaths.end());
        for (const Error& error : plugins.load(paths)) {
            m_err << "pi: plugin: " << error.message << "\n";
        }
    }

    int list(const ProviderLogin& login) {
        for (const std::string& provider : login.providers()) {
            std::string methods;
            for (const std::string& method : login.methods(provider)) {
                methods += (methods.empty() ? "" : ", ") + method;
            }
            m_out << provider << "\t" << methods << "\n";
        }
        return 0;
    }

    int status(ICredentialStore& credentials, const IClock& clock) {
        const auto infos = credentials.list();
        if (!infos) {
            m_err << "pi: " << infos.error().message << "\n";
            return 1;
        }
        if (infos->empty()) {
            m_out << "No credentials are stored.\n";
        }
        for (const CredentialInfo& info : *infos) {
            if (info.type == CredentialType::ApiKey) {
                m_out << info.providerId << "\tAPI key\n";
                continue;
            }
            const auto credential = credentials.read(info.providerId);
            std::string state = "OAuth";
            if (credential && credential->has_value() && (**credential).expires > 0 && (**credential).expires < 9e15) {
                state += (**credential).expires <= static_cast<double>(clock.nowMs()) ? " (access token expired; refreshed on use)" : " (access token valid)";
            }
            m_out << info.providerId << "\t" << state << "\n";
        }
        return 0;
    }

    int logout(ProviderLogin& login, const std::string& provider) {
        const auto removed = login.logout(provider);
        if (!removed) {
            m_err << "pi: " << removed.error().message << "\n";
            return 1;
        }
        m_out << "Signed out of \"" << provider << "\".\n";
        return 0;
    }

    int signIn(ProviderLogin& login, const CommandLine& line) {
        const std::string& provider = line.arguments[1];
        LoginInteraction interaction;
        interaction.manualOnly = line.loginManual;
        interaction.authUrl = [&](const std::string& url, const std::string& instructions) {
            m_out << "Open this URL in a browser to sign in to \"" << provider << "\":\n\n  " << url << "\n\n" << instructions << "\n";
            m_out.flush();
        };
        interaction.deviceCode = [&](const std::string& userCode, const std::string& uri, std::optional<std::int64_t>, std::optional<std::int64_t> expires) {
            m_out << "To sign in to \"" << provider << "\", open " << uri << " and enter the code\n\n  " << userCode << "\n\n";
            if (expires) {
                m_out << "The code expires in " << (*expires + 59) / 60 << " minutes. Waiting for approval...\n";
            }
            m_out.flush();
        };
        interaction.progress = [&](const std::string& message) { m_out << message << "\n"; };
        interaction.manualCode = [&](const std::string& message) -> std::optional<std::string> {
            m_out << message << "\n> ";
            m_out.flush();
            std::string input;
            if (!std::getline(m_in, input)) {
                return std::nullopt;
            }
            return input;
        };
        const auto done = login.login(provider, line.loginMethod, interaction);
        if (!done) {
            m_err << "pi: sign-in to \"" << provider << "\" failed: " << done.error().message << "\n";
            return 1;
        }
        m_out << "Signed in to \"" << provider << "\".\n";
        return 0;
    }

    CodingServices& m_services;
    IHttpClient& m_http;
    ISleeper& m_sleeper;
    OauthBrowserLogin::CallbackFactory m_callbacks;
    std::istream& m_in;
    std::ostream& m_out;
    std::ostream& m_err;
};
