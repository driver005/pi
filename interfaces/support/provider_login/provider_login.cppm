module;

#include <cstdint>

export module pi.support.provider_login;

import std;
export import pi.platform.i_base64_codec;
export import pi.platform.i_clock;
export import pi.platform.i_crypto;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.plugin.i_plugin_oauth;
export import pi.provider.i_credential_store;
export import pi.provider.i_oauth_flow;
export import pi.support.builtin_oauth_specs;
export import pi.support.oauth_browser_login;
export import pi.support.oauth_device_login;
export import pi.support.oauth_device_poller;
export import pi.support.oauth_token_mapper;
export import pi.support.provider_login_specs;
export import pi.support.radius_gateway;
export import pi.types.login_interaction;

/**
 * Signs in to a subscription provider and stores the credential in the credential store, in the shape the provider's refresh
 * keeps: Anthropic (browser or copy code), OpenAI Codex (browser or device code), Sign in with ChatGPT, OpenRouter (an API
 * key), xAI and Kimi Code (device code) and GitHub Copilot (device code, then the Copilot token through the provider's flow), Meta (device code, then the API key minted through the provider's flow) and Radius (browser with the gateway's discovered endpoint, or device code).
 * Sign-ins that follow the standard shapes are OauthBrowserLogin and OauthDeviceLogin over the descriptions of
 * ProviderLoginSpecs; the Codex device flow, which uses its own endpoints, and the Copilot token exchange are here. Port of the
 * login functions of packages/ai/src/auth/oauth.
 *
 * Errors are those of the flows; "unknown_provider" and "unknown_method" for requests this class cannot serve.
 */
export class ProviderLogin {
public:
    ProviderLogin(ICredentialStore& credentials, IHttpClient& http, const ICrypto& crypto, const IBase64Codec& base64, const IClock& clock, ISleeper& sleeper, OauthBrowserLogin::CallbackFactory callbacks, std::map<std::string, IOauthFlow*> flows, std::string kimiHost = "")
        : m_credentials(credentials),
          m_http(http),
          m_crypto(crypto),
          m_base64(base64),
          m_clock(clock),
          m_flows(std::move(flows)),
          m_kimiHost(std::move(kimiHost)),
          m_poller(clock, sleeper),
          m_browser(http, crypto, base64, std::move(callbacks)),
          m_device(http, m_poller) {}

    /** The Radius gateway origin the Radius sign-in uses (normalized, see RadiusGateway); without it the default gateway. */
    void setRadiusGateway(std::string gateway) {
        m_radiusGateway = std::move(gateway);
    }

    /** Adds the sign-ins plugins registered (method "plugin"); built-in providers keep theirs. */
    void setPlugins(IPluginOauth* plugins) {
        m_plugins = plugins;
    }

    std::vector<std::string> providers() const {
        std::vector<std::string> out = m_specs.providers();
        if (m_plugins != nullptr) {
            for (const auto& [provider, name] : m_plugins->oauthProviders()) {
                if (std::ranges::find(out, provider) == out.end()) {
                    out.push_back(provider);
                }
            }
        }
        return out;
    }

    /** The sign-in methods of a provider, the default first. */
    std::vector<std::string> methods(const std::string& provider) const {
        std::vector<std::string> built = m_specs.methods(provider);
        if (built.empty() && isPluginProvider(provider)) {
            return {"plugin"};
        }
        return built;
    }

    /** `method` empty selects the provider's default. The credential is stored when the sign-in succeeds. */
    Result<void> login(const std::string& provider, const std::string& method, const LoginInteraction& interaction, std::chrono::milliseconds timeout = OauthBrowserLogin::kDefaultTimeout) {
        const std::vector<std::string> offered = methods(provider);
        if (offered.empty()) {
            return std::unexpected(Error{"unknown_provider", "No sign-in is available for \"" + provider + "\""});
        }
        const std::string chosen = method.empty() ? offered.front() : method;
        if (std::ranges::find(offered, chosen) == offered.end()) {
            return std::unexpected(Error{"unknown_method", "\"" + provider + "\" does not sign in with \"" + chosen + "\""});
        }
        auto credential = chosen == "plugin" && isPluginProvider(provider) && m_specs.methods(provider).empty() ? m_plugins->oauthLogin(provider, interaction) : run(provider, chosen, interaction, timeout);
        if (!credential) {
            return std::unexpected(credential.error());
        }
        auto stored = m_credentials.modify(provider, [&](const std::optional<Credential>&) -> Result<std::optional<Credential>> { return std::optional<Credential>(*credential); });
        if (!stored) {
            return std::unexpected(stored.error());
        }
        return {};
    }

    Result<void> logout(const std::string& provider) {
        return m_credentials.remove(provider);
    }

private:
    bool isPluginProvider(const std::string& provider) const {
        if (m_plugins == nullptr) {
            return false;
        }
        const auto providers = m_plugins->oauthProviders();
        return std::ranges::any_of(providers, [&provider](const auto& entry) { return entry.first == provider; });
    }

    Result<Credential> run(const std::string& provider, const std::string& method, const LoginInteraction& interaction, std::chrono::milliseconds timeout) {
        if (method == "device_code") {
            return device(provider, interaction);
        }
        auto spec = m_specs.browser(provider);
        const auto mapper = mapperFor(provider);
        if (!spec || !mapper) {
            return std::unexpected(Error{"unknown_provider", "No browser sign-in is available for \"" + provider + "\""});
        }
        if (provider == "radius") {
            auto endpoint = radiusAuthorizationEndpoint(interaction.signal);
            if (!endpoint) {
                return std::unexpected(endpoint.error());
            }
            spec->authorizeUrl = *endpoint;
        }
        std::vector<OauthBrowserLogin::Param> extra;
        if (provider == "openai") {
            extra.emplace_back("ext_agent_host_id", "urn:uuid:" + uuid());
            extra.emplace_back("nonce", m_base64.encodeUrl(m_crypto.randomBytes(32)));
        }
        return m_browser.login(*spec, *mapper, extra, method == "copy_code", interaction, timeout);
    }

    Result<Credential> device(const std::string& provider, const LoginInteraction& interaction) {
        if (provider == "openai-codex") {
            return codexDevice(interaction);
        }
        const auto spec = m_specs.device(provider, m_kimiHost, radiusGateway());
        if (!spec) {
            return std::unexpected(Error{"unknown_provider", "No device sign-in is available for \"" + provider + "\""});
        }
        auto tokens = m_device.login(*spec, interaction);
        if (!tokens) {
            return std::unexpected(tokens.error());
        }
        if (provider == "github-copilot") {
            return copilot(*tokens, interaction);
        }
        if (provider == "meta") {
            return meta(*tokens, interaction);
        }
        const auto mapper = mapperFor(provider);
        if (!mapper) {
            return std::unexpected(Error{"unknown_provider", "No token mapping is available for \"" + provider + "\""});
        }
        return mapper->credentialFrom(*tokens, Credential{}, spec->clientId);
    }

    /** The identity token is the credential's `refresh`; the provider's flow mints the API key from it. */
    Result<Credential> meta(const Json& tokens, const LoginInteraction& interaction) {
        const auto flow = m_flows.find("meta");
        if (flow == m_flows.end() || flow->second == nullptr) {
            return std::unexpected(Error{"oauth", "Meta sign-in needs the provider's OAuth flow"});
        }
        if (interaction.progress) {
            interaction.progress("Enabling Meta Model API access...");
        }
        Credential identity;
        identity.type = CredentialType::OAuth;
        identity.refresh = tokens["access_token"].get<std::string>();
        return flow->second->refresh(identity, interaction.signal);
    }

    std::string radiusGateway() const {
        return m_radiusGateway.empty() ? RadiusGateway().normalize(RadiusGateway().defaultGateway()) : m_radiusGateway;
    }

    /** The browser authorization endpoint the Radius gateway announces at /v1/oauth. */
    Result<std::string> radiusAuthorizationEndpoint(const std::shared_ptr<AbortSignal>& signal) {
        const std::string gateway = radiusGateway();
        HttpRequest request;
        request.url = gateway + "/v1/oauth";
        request.headers = {{"Accept", "application/json"}};
        request.timeout = std::chrono::seconds(30);
        request.signal = signal;
        const auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(Error{"oauth", "Could not load Radius OAuth config from " + gateway + ": " + response.error().message});
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(Error{"oauth", "Could not load Radius OAuth config from " + gateway + ": " + std::to_string(response->status) + " " + response->body});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (!body.is_object() || !body.contains("authorizationEndpoint") || !body["authorizationEndpoint"].is_string()) {
            return std::unexpected(Error{"oauth", "Invalid Radius OAuth config from " + gateway});
        }
        return body["authorizationEndpoint"].get<std::string>();
    }

    /** The GitHub access token is the credential's `refresh`; the provider's flow turns it into a Copilot token. */
    Result<Credential> copilot(const Json& tokens, const LoginInteraction& interaction) {
        const auto flow = m_flows.find("github-copilot");
        if (flow == m_flows.end() || flow->second == nullptr) {
            return std::unexpected(Error{"oauth", "GitHub Copilot sign-in needs the provider's OAuth flow"});
        }
        if (interaction.progress) {
            interaction.progress("Fetching the Copilot token...");
        }
        Credential github;
        github.type = CredentialType::OAuth;
        github.refresh = tokens["access_token"].get<std::string>();
        return flow->second->refresh(github, interaction.signal);
    }

    /** The Codex device flow: its own endpoints hand out the authorization code and verifier the token endpoint then exchanges. */
    Result<Credential> codexDevice(const LoginInteraction& interaction) {
        const std::string base = "https://auth.openai.com";
        const std::string clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
        const auto mapper = mapperFor("openai-codex");
        if (!mapper) {
            return std::unexpected(Error{"oauth", "OpenAI Codex sign-in needs its token description"});
        }
        const auto started = m_http.send(json(base + "/api/accounts/deviceauth/usercode", Json{{"client_id", clientId}}, interaction.signal));
        if (!started) {
            return std::unexpected(Error{"oauth", "OpenAI Codex device code request failed: " + started.error().message});
        }
        if (started->status == 404) {
            return std::unexpected(Error{"oauth", "OpenAI Codex device code login is not enabled for this server. Use browser login or verify the server URL."});
        }
        if (started->status < 200 || started->status >= 300) {
            return std::unexpected(Error{"oauth", "OpenAI Codex device code request failed with status " + std::to_string(started->status) + (started->body.empty() ? "" : ": " + started->body)});
        }
        const Json device = Json::parse(started->body, nullptr, false);
        const std::optional<std::int64_t> interval = device.is_object() ? seconds(device.value("interval", Json())) : std::nullopt;
        if (!device.is_object() || !device.contains("device_auth_id") || !device["device_auth_id"].is_string() || !device.contains("user_code") || !device["user_code"].is_string() || !interval) {
            return std::unexpected(Error{"oauth", "Invalid OpenAI Codex device code response: " + started->body});
        }
        const std::string deviceAuthId = device["device_auth_id"].get<std::string>();
        const std::string userCode = device["user_code"].get<std::string>();
        constexpr std::int64_t kExpires = 15 * 60;
        if (interaction.deviceCode) {
            interaction.deviceCode(userCode, base + "/codex/device", interval, kExpires);
        }
        auto granted = m_poller.poll(interval, kExpires, false, interaction.signal, [&] { return codexPoll(base, deviceAuthId, userCode, interaction.signal); });
        if (!granted) {
            return std::unexpected(granted.error());
        }
        const auto exchanged = m_http.send(mapper->tokenRequest({{"grant_type", "authorization_code"},
                                                                 {"client_id", clientId},
                                                                 {"code", (*granted)["authorization_code"].get<std::string>()},
                                                                 {"code_verifier", (*granted)["code_verifier"].get<std::string>()},
                                                                 {"redirect_uri", base + "/deviceauth/callback"}},
                                                                interaction.signal));
        if (!exchanged) {
            return std::unexpected(Error{"oauth", "OpenAI Codex token exchange request failed: " + exchanged.error().message});
        }
        if (exchanged->status < 200 || exchanged->status >= 300) {
            return std::unexpected(Error{"oauth", "OpenAI Codex token exchange failed (" + std::to_string(exchanged->status) + "): " + exchanged->body.substr(0, 500)});
        }
        const Json body = Json::parse(exchanged->body, nullptr, false);
        if (!body.is_object()) {
            return std::unexpected(Error{"oauth", "OpenAI Codex token exchange returned invalid JSON"});
        }
        return mapper->credentialFrom(body, Credential{}, clientId);
    }

    DevicePollResult codexPoll(const std::string& base, const std::string& deviceAuthId, const std::string& userCode, const std::shared_ptr<AbortSignal>& signal) {
        DevicePollResult result;
        const auto response = m_http.send(json(base + "/api/accounts/deviceauth/token", Json{{"device_auth_id", deviceAuthId}, {"user_code", userCode}}, signal));
        if (!response) {
            result.status = "failed";
            result.message = "OpenAI Codex device auth request failed: " + response.error().message;
            return result;
        }
        const Json body = Json::parse(response->body, nullptr, false);
        if (response->status >= 200 && response->status < 300) {
            if (!body.is_object() || !body.contains("authorization_code") || !body["authorization_code"].is_string() || !body.contains("code_verifier") || !body["code_verifier"].is_string()) {
                result.status = "failed";
                result.message = "Invalid OpenAI Codex device auth token response: " + response->body;
                return result;
            }
            result.status = "complete";
            result.value = body;
            return result;
        }
        if (response->status == 403 || response->status == 404) {
            return result;
        }
        std::string code;
        if (body.is_object() && body.contains("error")) {
            const Json& error = body["error"];
            code = error.is_string() ? error.get<std::string>() : error.is_object() && error.contains("code") && error["code"].is_string() ? error["code"].get<std::string>() : std::string();
        }
        if (code == "deviceauth_authorization_pending") {
            return result;
        }
        if (code == "slow_down") {
            result.status = "slow_down";
            return result;
        }
        result.status = "failed";
        result.message = "OpenAI Codex device auth failed with status " + std::to_string(response->status) + (response->body.empty() ? "" : ": " + response->body);
        return result;
    }

    HttpRequest json(const std::string& url, const Json& body, const std::shared_ptr<AbortSignal>& signal) const {
        HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
        request.body = body.dump();
        request.timeout = std::chrono::seconds(30);
        request.signal = signal;
        return request;
    }

    /** A number of seconds given as a number or a numeric string. */
    std::optional<std::int64_t> seconds(const Json& value) const {
        if (value.is_number() && value.get<double>() >= 0) {
            return static_cast<std::int64_t>(value.get<double>());
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            std::int64_t parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error == std::errc() && end == text.data() + text.size() && parsed >= 0) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    std::optional<OauthTokenMapper> mapperFor(const std::string& provider) const {
        if (provider == "radius") {
            return OauthTokenMapper(BuiltinOauthSpecs().radius(radiusGateway()), m_clock, m_base64);
        }
        for (OauthRefreshSpec& spec : BuiltinOauthSpecs().all(m_kimiHost)) {
            if (spec.providerId == provider) {
                return OauthTokenMapper(std::move(spec), m_clock, m_base64);
            }
        }
        return std::nullopt;
    }

    /** A random (version 4) UUID. */
    std::string uuid() const {
        std::string bytes = m_crypto.randomBytes(16);
        bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0F) | 0x40);
        bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3F) | 0x80);
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            out += std::format("{:02x}", static_cast<unsigned char>(bytes[i]));
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                out += '-';
            }
        }
        return out;
    }

    ICredentialStore& m_credentials;
    IHttpClient& m_http;
    const ICrypto& m_crypto;
    const IBase64Codec& m_base64;
    const IClock& m_clock;
    std::map<std::string, IOauthFlow*> m_flows;
    std::string m_kimiHost;
    std::string m_radiusGateway;
    ProviderLoginSpecs m_specs;
    IPluginOauth* m_plugins = nullptr;
    OauthDevicePoller m_poller;
    OauthBrowserLogin m_browser;
    OauthDeviceLogin m_device;
};
