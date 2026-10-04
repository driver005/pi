module;

#include <cstdint>

export module pi.support.virtual_model_registry;

import std;
export import pi.support.thinking_level_resolver;
export import pi.support.virtual_model_names;
export import pi.types.model;
export import pi.types.result;
export import pi.types.virtual_model_definition;
export import pi.types.virtual_resolve_request;

/**
 * The virtual models a model runtime lists next to its physical ones, and the routing of their requests. A virtual model is a
 * catalog entry whose api is `pi-virtual`: providers never see it, a router maps each request to a physical model. Port of
 * core/virtual-models.ts and the virtual model parts of model-runtime.ts.
 *
 * The runtime supplies two views of itself: `physical` finds a catalog model that is not virtual, `configured` tells whether a
 * provider has credentials.
 */
export class VirtualModelRegistry {
public:
    using Physical = std::function<std::optional<Model>(const std::string& provider, const std::string& id)>;
    using Configured = std::function<bool(const std::string& provider)>;

    std::string api() const {
        return m_names.api();
    }

    std::string stateEntryType() const {
        return m_names.stateEntryType();
    }

    bool isVirtual(const Model& model) const {
        return m_names.isVirtual(model);
    }

    /** Registers or replaces a virtual model. `isPhysicalId` tells whether the provider has a physical model of that id. */
    Result<void> add(VirtualModelDefinition definition, const std::function<bool(const std::string&, const std::string&)>& isPhysicalId) {
        if (definition.provider.find_first_not_of(" \t\r\n") == std::string::npos || definition.id.find_first_not_of(" \t\r\n") == std::string::npos) {
            return std::unexpected(Error{"virtual_model", "Virtual model provider and id must not be empty."});
        }
        if (!definition.route) {
            return std::unexpected(Error{"virtual_model", "Virtual model " + definition.provider + "/" + definition.id + " needs a route function."});
        }
        if (isPhysicalId(definition.provider, definition.id)) {
            return std::unexpected(Error{"virtual_model", "Virtual model " + definition.provider + "/" + definition.id + " conflicts with a physical model."});
        }
        const std::pair<std::string, std::string> key{definition.provider, definition.id};
        Model model = catalogEntry(definition);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_models[key] = std::move(model);
        m_routes[key] = std::move(definition.route);
        return {};
    }

    /** Whether a virtual model was registered under that provider and id. */
    bool remove(const std::string& provider, const std::string& id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_routes.erase({provider, id});
        return m_models.erase({provider, id}) > 0;
    }

    std::vector<Model> models() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Model> out;
        for (const auto& [key, model] : m_models) {
            out.push_back(model);
        }
        return out;
    }

    std::optional<Model> find(const std::string& provider, const std::string& id) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_models.find({provider, id});
        return found == m_models.end() ? std::nullopt : std::optional<Model>(found->second);
    }

    bool listsProvider(const std::string& provider) const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return std::ranges::any_of(m_models, [&](const auto& entry) { return entry.first.first == provider; });
    }

    std::set<std::string> providers() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::set<std::string> out;
        for (const auto& [key, model] : m_models) {
            out.insert(key.first);
        }
        return out;
    }

    /**
     * Asks the router of `request.model` for one request. The answer must name a physical catalog model of a provider with
     * credentials; its thinking level is clamped to that model.
     */
    Result<VirtualRoute> resolve(const VirtualResolveRequest& request, const Physical& physical, const Configured& configured) const {
        const std::string name = "Virtual model " + request.model.provider + "/" + request.model.id;
        std::function<Result<VirtualRoute>(const VirtualRouteRequest&)> route;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_routes.find({request.model.provider, request.model.id});
            if (found == m_routes.end()) {
                return std::unexpected(Error{"virtual_model", name + " is not registered."});
            }
            route = found->second;
        }
        VirtualRouteRequest routed;
        routed.model = request.model;
        routed.thinkingLevel = request.thinkingLevel;
        routed.reason = request.reason;
        routed.state = request.state;
        routed.messages = request.messages;
        routed.signal = request.signal;
        if (const AssistantMessage* latest = latestResponse(request.messages)) {
            if (auto model = physical(latest->provider, latest->model)) {
                routed.previous = RoutedSelection{std::move(*model), latest->thinkingLevel};
            }
        }
        if (request.failed) {
            if (auto model = physical(request.failed->provider, request.failed->model)) {
                routed.failed = FailedSelection{std::move(*model), request.failed->thinkingLevel, *request.failed};
            }
        }
        auto answer = route(routed);
        if (!answer) {
            return std::unexpected(Error{"virtual_model", name + " could not route: " + answer.error().message});
        }
        const std::string target = answer->model.provider + "/" + answer->model.id;
        auto model = physical(answer->model.provider, answer->model.id);
        if (!model) {
            return std::unexpected(Error{"virtual_model", name + " routed to " + target + ", which is not a physical model."});
        }
        if (!configured(model->provider)) {
            return std::unexpected(Error{"virtual_model", name + " routed to " + target + ", which has no credentials."});
        }
        VirtualRoute out;
        out.thinkingLevel = m_levels.clamp(*model, answer->thinkingLevel);
        out.model = std::move(*model);
        out.state = std::move(answer->state);
        return out;
    }

    /** The latest assistant message that neither failed nor was aborted: its model is physical. */
    const AssistantMessage* latestResponse(const std::vector<Message>& messages) const {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&*it)) {
                if (assistant->stopReason != StopReason::Error && assistant->stopReason != StopReason::Aborted) {
                    return assistant;
                }
            }
        }
        return nullptr;
    }

    /** The catalog entry of a virtual model: thinking levels the router accepts, no cost, no provider wire API. */
    Model catalogEntry(const VirtualModelDefinition& definition) const {
        const std::vector<ThinkingLevel> levels = definition.thinkingLevels.empty() ? std::vector<ThinkingLevel>{ThinkingLevel::Off} : definition.thinkingLevels;
        Model model;
        model.id = definition.id;
        model.name = definition.name;
        model.api = api();
        model.provider = definition.provider;
        model.input = definition.input.empty() ? std::vector<std::string>{"text", "image"} : definition.input;
        model.contextWindow = definition.contextWindow;
        model.maxTokens = definition.maxTokens;
        Json map = Json::object();
        bool reasoning = false;
        for (const ThinkingLevel level : {ThinkingLevel::Off, ThinkingLevel::Minimal, ThinkingLevel::Low, ThinkingLevel::Medium, ThinkingLevel::High, ThinkingLevel::XHigh, ThinkingLevel::Max}) {
            const bool offered = std::ranges::find(levels, level) != levels.end();
            map[m_levels.levelName(level)] = offered ? Json(m_levels.levelName(level)) : Json();
            reasoning = reasoning || (offered && level != ThinkingLevel::Off);
        }
        model.thinkingLevelMap = map;
        model.reasoning = reasoning;
        return model;
    }

private:
    VirtualModelNames m_names;
    ThinkingLevelResolver m_levels;
    mutable std::mutex m_mutex;
    std::map<std::pair<std::string, std::string>, Model> m_models;
    std::map<std::pair<std::string, std::string>, std::function<Result<VirtualRoute>(const VirtualRouteRequest&)>> m_routes;
};
