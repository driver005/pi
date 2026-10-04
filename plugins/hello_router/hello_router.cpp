// An example plugin: registers the virtual model "hello-router/auto". Requests that follow a user message go to the small model
// of the first provider that has credentials ("high" asks for its reasoning model, when it has one); continuations and retries
// stay on the model that answered before. The routing phase is kept as router state on the session branch.
#include "pi_plugin.hpp"

class HelloRouterPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        m_host = &host;
        const std::string error = host.registerVirtualModel(
            pi::Json{{"provider", "hello-router"},
                     {"id", "auto"},
                     {"name", "Auto"},
                     {"thinkingLevels", pi::Json::array({"low", "high"})},
                     {"contextWindow", 128000},
                     {"maxTokens", 8192}},
            [this](const pi::RouteCall& call) { return route(call.request); });
        if (!error.empty()) {
            host.log(PI_LOG_ERROR, error);
            return false;
        }
        return true;
    }

private:
    pi::Json route(const pi::Json& request) const {
        const std::string reason = request.value("reason", "user");
        const bool sticky = reason == "continuation" || reason == "retry";
        const pi::Json sameModel = sticky ? stickyModel(request) : pi::Json();
        if (sameModel.is_object()) {
            return pi::Json{{"model", sameModel}, {"thinkingLevel", stickyLevel(request)}};
        }
        const pi::Json models = m_host->models();
        const bool wantsReasoning = request.value("thinkingLevel", "off") == "high";
        pi::Json chosen;
        for (const pi::Json& model : models) {
            if (model.value("reasoning", false) == wantsReasoning) {
                chosen = model;
                break;
            }
        }
        if (chosen.is_null() && !models.empty()) {
            chosen = models.front();
        }
        if (chosen.is_null()) {
            return pi::failure("no model with credentials to route to");
        }
        return pi::Json{{"model", {{"provider", chosen["provider"]}, {"id", chosen["id"]}}},
                        {"thinkingLevel", wantsReasoning ? "medium" : "off"},
                        {"state", {{"routed", request.value("state", pi::Json::object()).value("routed", 0) + 1}}}};
    }

    pi::Json stickyModel(const pi::Json& request) const {
        const char* key = reason(request) == "retry" && request.contains("failed") ? "failed" : "previous";
        if (request.contains(key) && request[key].contains("model")) {
            const pi::Json& model = request[key]["model"];
            return pi::Json{{"provider", model["provider"]}, {"id", model["id"]}};
        }
        return pi::Json();
    }

    std::string stickyLevel(const pi::Json& request) const {
        const char* key = reason(request) == "retry" && request.contains("failed") ? "failed" : "previous";
        return request.contains(key) ? request[key].value("thinkingLevel", "off") : "off";
    }

    std::string reason(const pi::Json& request) const { return request.value("reason", "user"); }

    pi::Host* m_host = nullptr;
};

PI_PLUGIN(HelloRouterPlugin)
