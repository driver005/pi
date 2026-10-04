export module pi.support.builtin_documents;

import std;
export import pi.types.doc_definition;
export import pi.types.json;

/**
 * The built-in conversation documents of the durable harness: `pi.agent`, `pi.live`, `pi.inbox` and `pi.usage`.
 * Port of AgentDoc, LiveDoc, InboxDoc and UsageDoc.
 */
export class BuiltinDocuments {
public:
    /** The stored choices of one conversation; rewindable so forks start from the agent at their fork entry. */
    DocDefinition agent() const {
        DocDefinition definition = conversationDocument("pi.agent", "rewindable", "asOf");
        definition.initial = [](const Json&) { return Json::object(); };
        definition.checkpointWhen = [](const Json&, const Json&, std::int64_t) { return true; };
        return definition;
    }

    /** Run control and presentation of the current generation and tool round. */
    DocDefinition live() const {
        DocDefinition definition = conversationDocument("pi.live", "latest", "initial");
        definition.initial = [](const Json&) { return Json::object(); };
        // A complete base whenever nothing runs: no generation and no running tool slot, so the delta chain spans at
        // most one generation or the overlapping execution of one round's tools.
        definition.checkpointWhen = [](const Json& value, const Json&, std::int64_t) {
            if (value.contains("generation")) {
                return false;
            }
            if (value.contains("tools")) {
                for (const Json& slot : value.at("tools")) {
                    if (slot.value("status", std::string()) == "running") {
                        return false;
                    }
                }
            }
            return true;
        };
        return definition;
    }

    /** The queue of one conversation's submissions waiting for a boundary, in id order. */
    DocDefinition inbox() const {
        DocDefinition definition = conversationDocument("pi.inbox", "latest", "initial");
        definition.initial = [](const Json&) { return Json::object({{"items", Json::array()}}); };
        definition.checkpointWhen = [](const Json& value, const Json&, std::int64_t) { return value.at("items").empty(); };
        return definition;
    }

    /** Ledger of one conversation's own spend. */
    DocDefinition usage() const {
        DocDefinition definition = conversationDocument("pi.usage", "latest", "initial");
        definition.initial = [](const Json&) { return Json::object({{"models", Json::object()}, {"tools", Json::object()}}); };
        definition.checkpointWhen = [](const Json&, const Json&, std::int64_t) { return true; };
        return definition;
    }

private:
    DocDefinition conversationDocument(const std::string& kind, const std::string& history, const std::string& fork) const {
        DocDefinition definition;
        definition.kind = kind;
        definition.version = 1;
        definition.scope = "conversation";
        definition.history = history;
        definition.fork = fork;
        return definition;
    }
};
