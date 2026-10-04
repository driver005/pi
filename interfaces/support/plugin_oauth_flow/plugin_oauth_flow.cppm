export module pi.support.plugin_oauth_flow;

import std;
export import pi.provider.i_oauth_flow;
export import pi.types.login_interaction;

/**
 * The OAuth sign-in of a plugin provider: `login` runs the plugin's sign-in and `refresh` its token exchange, both as JSON
 * credentials ({access, refresh, expires} in epoch milliseconds, other fields kept with the credential and handed back on
 * refresh); the `access` token is the provider's API key. Calls into the plugin are counted so close() can wait for them before
 * the plugin is unloaded; after close() every call fails.
 */
export class PluginOauthFlow : public IOauthFlow {
public:
    using Login = std::function<Result<Json>(const LoginInteraction& interaction)>;
    using Refresh = std::function<Result<Json>(const Json& credential, const std::shared_ptr<AbortSignal>& signal)>;

    PluginOauthFlow(std::string provider, std::string name, bool subscription, Login login, Refresh refresh)
        : m_provider(std::move(provider)),
          m_name(std::move(name)),
          m_subscription(subscription),
          m_login(std::move(login)),
          m_refresh(std::move(refresh)) {}

    std::string providerId() const override {
        return m_provider;
    }

    std::string name() const override {
        return m_name;
    }

    bool isSubscription() const override {
        return m_subscription;
    }

    /** Signs in through the plugin; the credential still has to be stored. */
    Result<Credential> login(const LoginInteraction& interaction) {
        if (const auto entered = enter(); !entered) {
            return std::unexpected(entered.error());
        }
        const Result<Json> answer = m_login(interaction);
        leave();
        return credentialFrom(answer, "sign-in");
    }

    Result<Credential> refresh(const Credential& credential, const std::shared_ptr<AbortSignal>& signal) override {
        if (const auto entered = enter(); !entered) {
            return std::unexpected(entered.error());
        }
        const Result<Json> answer = m_refresh(credentialJson(credential), signal);
        leave();
        return credentialFrom(answer, "token refresh");
    }

    ModelAuth toAuth(const Credential& credential) const override {
        ModelAuth auth;
        auth.apiKey = credential.access;
        return auth;
    }

    /** Waits for the calls in progress and refuses later ones. */
    void close() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_closed = true;
        m_idle.wait(lock, [this] { return m_running == 0; });
    }

private:
    Result<void> enter() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(Error{"oauth", "The plugin of the " + m_provider + " sign-in is no longer loaded"});
        }
        ++m_running;
        return {};
    }

    void leave() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        --m_running;
        m_idle.notify_all();
    }

    Json credentialJson(const Credential& credential) const {
        Json out = credential.extra.is_object() ? credential.extra : Json::object();
        out["access"] = credential.access;
        out["refresh"] = credential.refresh;
        out["expires"] = credential.expires;
        return out;
    }

    Result<Credential> credentialFrom(const Result<Json>& answer, const std::string& what) const {
        if (!answer) {
            return std::unexpected(answer.error());
        }
        if (!answer->is_object()) {
            return std::unexpected(Error{"oauth", "The " + m_provider + " " + what + " did not return a credential"});
        }
        if (answer->contains("error")) {
            return std::unexpected(Error{"oauth", (*answer)["error"].is_string() ? (*answer)["error"].get<std::string>() : "The " + m_provider + " " + what + " failed"});
        }
        const Json& json = *answer;
        if (!json.contains("access") || !json["access"].is_string() || !json.contains("refresh") || !json["refresh"].is_string() || !json.contains("expires") || !json["expires"].is_number()) {
            return std::unexpected(Error{"oauth", "The " + m_provider + " " + what + " must return {access, refresh, expires}"});
        }
        Credential credential;
        credential.type = CredentialType::OAuth;
        credential.access = json["access"].get<std::string>();
        credential.refresh = json["refresh"].get<std::string>();
        credential.expires = json["expires"].get<double>();
        for (const auto& entry : json.items()) {
            if (entry.key() != "access" && entry.key() != "refresh" && entry.key() != "expires") {
                credential.extra[entry.key()] = entry.value();
            }
        }
        return credential;
    }

    std::string m_provider;
    std::string m_name;
    bool m_subscription;
    Login m_login;
    Refresh m_refresh;
    std::mutex m_mutex;
    std::condition_variable m_idle;
    int m_running = 0;
    bool m_closed = false;
};
