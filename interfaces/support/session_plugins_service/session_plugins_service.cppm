export module pi.support.session_plugins_service;

import std;
export import pi.chord.i_remote_service;

/**
 * The `pi.session-plugins` service of a session: `reload` makes the session's host load its plugins again, so plugins
 * installed or changed on disk take effect without restarting the session. What reloading means is up to the host that
 * provides the callback. Port of the SessionPlugins service in experimental/services/worker.ts.
 */
export class SessionPluginsService : public IRemoteService {
public:
    using Reload = std::function<Result<void>()>;

    explicit SessionPluginsService(Reload reload)
        : m_reload(std::move(reload)) {}

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["reload"] = [this](const std::vector<Json>& args, const ServiceContext&) -> Result<std::optional<Json>> {
            if (!args.empty()) {
                return std::unexpected(Error{"invalid_request", "reload takes no arguments"});
            }
            const std::lock_guard<std::mutex> serial(m_mutex);
            if (auto reloaded = m_reload(); !reloaded) {
                return std::unexpected(reloaded.error());
            }
            return std::optional<Json>();
        };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {};
    }

private:
    Reload m_reload;
    std::mutex m_mutex;
};
