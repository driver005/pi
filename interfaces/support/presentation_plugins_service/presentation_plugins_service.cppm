export module pi.support.presentation_plugins_service;

import std;
export import pi.chord.i_remote_service;
export import pi.server.i_session_catalog;

/**
 * The `pi.presentation-plugins` service of one client: the presentation plugins its UI should load for a session
 * (`prepareSession`) and again after a plugin reload (`reload`). Presentation plugins are JavaScript bundles of the TS
 * server; this server hosts native plugins only, so both calls answer the empty selection
 * `{presentationFacetBundles: []}` after checking their preconditions as the TS service does: the session must exist, and
 * `reload` needs a prepared selection, which detaching the client clears. Calls run under the server-wide mutation lock.
 * Port of the PresentationPlugins provider in experimental/services/server.ts.
 */
export class PresentationPluginsService : public IRemoteService {
public:
    PresentationPluginsService(ISessionCatalog& catalog, std::mutex& mutations)
        : m_catalog(catalog),
          m_mutations(mutations) {}

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["prepareSession"] = [this](const std::vector<Json>& args, const ServiceContext&) { return prepareSession(args); };
        methods["reload"] = [this](const std::vector<Json>& args, const ServiceContext&) { return reload(args); };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {};
    }

    /** Forgets the prepared selection; the caller already holds the mutation lock. */
    void clearPrepared() {
        m_prepared = false;
    }

private:
    Result<std::optional<Json>> prepareSession(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_object() || !args[0].contains("sessionId") || !args[0]["sessionId"].is_string()) {
            return std::unexpected(Error{"invalid_request", "Expected {sessionId, packagePaths}"});
        }
        const Json paths = args[0].value("packagePaths", Json());
        if (!paths.is_null() && !paths.is_array()) {
            return std::unexpected(Error{"invalid_request", "packagePaths must be an array of strings or null"});
        }
        for (const Json& path : paths) {
            if (!path.is_string()) {
                return std::unexpected(Error{"invalid_request", "packagePaths must be an array of strings or null"});
            }
        }
        const std::lock_guard<std::mutex> serial(m_mutations);
        if (auto record = m_catalog.resolve(args[0]["sessionId"].get<std::string>()); !record) {
            return std::unexpected(record.error());
        }
        m_prepared = true;
        return std::optional<Json>(selection());
    }

    Result<std::optional<Json>> reload(const std::vector<Json>& args) {
        if (!args.empty()) {
            return std::unexpected(Error{"invalid_request", "reload takes no arguments"});
        }
        const std::lock_guard<std::mutex> serial(m_mutations);
        if (!m_prepared) {
            return std::unexpected(Error{"invalid_state", "No Session plugin selection is prepared"});
        }
        return std::optional<Json>(selection());
    }

    Json selection() const {
        return Json{{"presentationFacetBundles", Json::array()}};
    }

    ISessionCatalog& m_catalog;
    std::mutex& m_mutations;
    bool m_prepared = false;
};
