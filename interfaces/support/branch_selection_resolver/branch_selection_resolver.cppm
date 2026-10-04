export module pi.support.branch_selection_resolver;

import std;
export import pi.support.virtual_model_names;
export import pi.types.model;
export import pi.types.session_entry;
export import pi.types.session_model_ref;

/**
 * The model selection a session branch records. A virtual `model_change` holds until the next `model_change`, because the
 * responses name the physical models it routed to; otherwise the latest physical response wins, as in sessions without virtual
 * models. A virtual model that is no longer registered does not hold, so the selection falls back to the physical model that
 * answered last. Port of getBranchSelection in core/virtual-models.ts.
 */
export class BranchSelectionResolver {
public:
    /** Finds a catalog model, virtual ones included; nullopt when it is not registered. */
    using Find = std::function<std::optional<Model>(const std::string& provider, const std::string& modelId)>;

    std::optional<SessionModelRef> resolve(const std::vector<SessionEntry>& branch, const Find& find) const {
        for (std::size_t i = branch.size(); i-- > 0;) {
            const SessionEntry& entry = branch[i];
            if (entry.type == "model_change") {
                return change(entry);
            }
            if (entry.type != "message" || !isAssistant(entry) || isVirtual(entry.body["message"])) {
                continue;
            }
            const Json& message = entry.body["message"];
            SessionModelRef response{message.value("provider", ""), message.value("model", "")};
            const auto held = lastChange(branch, i);
            if (held) {
                const auto model = find(held->provider, held->modelId);
                if (model && m_names.isVirtual(*model)) {
                    return held;
                }
            }
            return response;
        }
        return std::nullopt;
    }

private:
    VirtualModelNames m_names;

    bool isAssistant(const SessionEntry& entry) const {
        return entry.body.contains("message") && entry.body["message"].is_object() && entry.body["message"].value("role", "") == "assistant";
    }

    bool isVirtual(const Json& message) const {
        return m_names.isVirtualApi(message.value("api", ""));
    }

    SessionModelRef change(const SessionEntry& entry) const {
        return SessionModelRef{entry.body.value("provider", ""), entry.body.value("modelId", "")};
    }

    std::optional<SessionModelRef> lastChange(const std::vector<SessionEntry>& branch, std::size_t before) const {
        for (std::size_t i = before; i-- > 0;) {
            if (branch[i].type == "model_change") {
                return change(branch[i]);
            }
        }
        return std::nullopt;
    }
};
