export module pi.plugin.i_plugin_oauth;

import std;
export import pi.types.credential;
export import pi.types.login_interaction;
export import pi.types.result;

/** The provider sign-ins plugins registered (register_oauth), as `pi auth` sees them. */
export class IPluginOauth {
public:
    virtual ~IPluginOauth() = default;

    /** (provider id, display name) of every plugin sign-in. */
    virtual std::vector<std::pair<std::string, std::string>> oauthProviders() const = 0;
    /** Runs the plugin's sign-in; the credential is not stored. Error "unknown_provider" when no plugin registered it. */
    virtual Result<Credential> oauthLogin(const std::string& provider, const LoginInteraction& interaction) = 0;
};
