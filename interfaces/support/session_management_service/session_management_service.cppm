export module pi.support.session_management_service;

import std;
export import pi.chord.i_remote_service;
export import pi.server.i_server_presentation;
export import pi.support.session_directory_service;

/**
 * The `pi.session-management` service of one client: create and remove sessions in the catalog and
 * attach or detach the client's own session. Calls of all clients run one at a time. Port of the
 * SessionManagement provider in experimental/services/server.ts.
 */
export class SessionManagementService : public IRemoteService {
public:
    /** `onDetached` runs after the client's session detached, under the mutation lock. */
    SessionManagementService(IServerPresentation& presentation, ISessionCatalog& catalog, SessionDirectoryService& directory, std::function<void()> onDetached = nullptr)
        : m_presentation(presentation),
          m_catalog(catalog),
          m_directory(directory),
          m_onDetached(std::move(onDetached)) {}

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["create"] = [this](const std::vector<Json>& args, const ServiceContext& context) {
            return create(args, context);
        };
        methods["remove"] = [this](const std::vector<Json>& args, const ServiceContext& context) {
            return remove(args, context);
        };
        methods["attach"] = [this](const std::vector<Json>& args, const ServiceContext& context) {
            return attach(args, context);
        };
        methods["detach"] = [this](const std::vector<Json>&, const ServiceContext& context) { return detach(context); };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {};
    }

private:
    Result<std::optional<Json>> create(const std::vector<Json>& args, const ServiceContext& context) {
        std::optional<std::string> id;
        if (args.size() != 1 || !args[0].is_object()) {
            return std::unexpected(Error{"invalid_request", "Invalid session create options"});
        }
        const Json requested = args[0].value("id", Json());
        if (requested.is_string()) {
            id = requested.get<std::string>();
        } else if (!requested.is_null()) {
            return std::unexpected(Error{"invalid_request", "Invalid session create options"});
        }
        const std::lock_guard<std::mutex> serial(m_directory.mutations());
        auto created = m_catalog.create(id);
        if (!created) {
            return std::unexpected(created.error());
        }
        if (auto refreshed = m_directory.refresh(context); !refreshed) {
            return std::unexpected(refreshed.error());
        }
        return std::optional<Json>(m_directory.summary(*created));
    }

    Result<std::optional<Json>> remove(const std::vector<Json>& args, const ServiceContext& context) {
        auto requested = sessionArgument(args);
        if (!requested) {
            return std::unexpected(requested.error());
        }
        const std::lock_guard<std::mutex> serial(m_directory.mutations());
        auto record = m_catalog.resolve(*requested);
        if (!record) {
            return std::unexpected(record.error());
        }
        if (auto prepared = m_presentation.prepareSessionRemoval(record->id, context); !prepared) {
            return std::unexpected(prepared.error());
        }
        if (auto removed = m_catalog.remove(record->id); !removed) {
            return std::unexpected(removed.error());
        }
        if (auto refreshed = m_directory.refresh(context); !refreshed) {
            return std::unexpected(refreshed.error());
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> attach(const std::vector<Json>& args, const ServiceContext& context) {
        auto requested = sessionArgument(args);
        if (!requested) {
            return std::unexpected(requested.error());
        }
        const std::lock_guard<std::mutex> serial(m_directory.mutations());
        if (auto attached = m_presentation.attachSession(*requested, context); !attached) {
            return std::unexpected(attached.error());
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> detach(const ServiceContext& context) {
        const std::lock_guard<std::mutex> serial(m_directory.mutations());
        if (auto detached = m_presentation.detachSession(context); !detached) {
            return std::unexpected(detached.error());
        }
        if (m_onDetached) {
            m_onDetached();
        }
        return std::optional<Json>();
    }

    Result<std::string> sessionArgument(const std::vector<Json>& args) const {
        if (args.size() != 1 || !args[0].is_string() || args[0].get_ref<const std::string&>().empty()) {
            return std::unexpected(Error{"invalid_request", "Expected one session id"});
        }
        return args[0].get<std::string>();
    }

    IServerPresentation& m_presentation;
    ISessionCatalog& m_catalog;
    SessionDirectoryService& m_directory;
    std::function<void()> m_onDetached;
};
