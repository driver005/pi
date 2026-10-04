module;

#include <cstdint>

export module pi.support.agent_configurator;

import std;
export import pi.support.builtin_documents;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;

/**
 * Edits of a conversation's stored agent (`pi.agent`). A change is a JSON object with any of `model`
 * (`{provider, modelId}`), `thinkingLevel`, `extensions` (a name list, or `{add, remove}` edits of the host
 * default), `tools` (a name list, or `{remove}`), `instructions` and `cwd`: a given field replaces the stored one,
 * `null` clears it, an absent one changes nothing. Port of the commit-side half of agent.ts.
 */
export class AgentConfigurator {
public:
    /** Applies one change to `pi.agent`. */
    Result<void> configure(Transaction& tx, std::int64_t conversationId, const Json& change) const {
        auto state = tx.doc(m_documents.agent(), owner(conversationId));
        if (!state) {
            return std::unexpected(state.error());
        }
        for (const char* key : {"model", "thinkingLevel", "extensions", "tools", "instructions", "cwd"}) {
            if (!change.contains(key)) {
                continue;
            }
            if (change.at(key).is_null()) {
                (*state)->erase(key);
            } else {
                (**state)[key] = change.at(key);
            }
        }
        return {};
    }

    /**
     * `addTools` of a tool round: a list gets each name it lacks appended, `{remove}` loses the names, and unset
     * tools already offer every tool, so nothing is written.
     */
    Result<void> addTools(Transaction& tx, std::int64_t conversationId, const std::vector<std::string>& added) const {
        auto state = tx.doc(m_documents.agent(), owner(conversationId));
        if (!state) {
            return std::unexpected(state.error());
        }
        if (!(*state)->contains("tools")) {
            return {};
        }
        Json& tools = (**state)["tools"];
        if (tools.is_array()) {
            for (const std::string& name : added) {
                if (std::find(tools.begin(), tools.end(), Json(name)) == tools.end()) {
                    tools.push_back(name);
                }
            }
            return {};
        }
        Json kept = Json::array();
        bool changed = false;
        for (const Json& name : tools.at("remove")) {
            if (std::find(added.begin(), added.end(), name.get<std::string>()) == added.end()) {
                kept.push_back(name);
            } else {
                changed = true;
            }
        }
        if (changed) {
            tools = Json::object({{"remove", kept}});
        }
        return {};
    }

    /**
     * The built-in `pi.agent` part of every commit that creates or forks a conversation: a fork keeps its `asOf` copy; a
     * new task-owned conversation copies the stored agent of its owner task's conversation; a new ownerless one starts
     * empty.
     */
    Result<void> createAgent(Transaction& tx, const Json& conversation) const {
        if (conversation.contains("parent")) {
            return {};
        }
        auto agent = tx.doc(m_documents.agent(), owner(conversation.at("id").get<std::int64_t>()));
        if (!agent) {
            return std::unexpected(agent.error());
        }
        if (!conversation.contains("owner")) {
            return {};
        }
        auto from = tx.doc(m_documents.agent(), owner(conversation.at("owner").at("conversationId").get<std::int64_t>()));
        if (!from) {
            return std::unexpected(from.error());
        }
        for (const auto& entry : (*from)->items()) {
            (**agent)[entry.key()] = entry.value();
        }
        return {};
    }

private:
    DocAddressArgs owner(std::int64_t conversationId) const {
        DocAddressArgs args;
        args.owner = conversationId;
        return args;
    }

    BuiltinDocuments m_documents;
};
