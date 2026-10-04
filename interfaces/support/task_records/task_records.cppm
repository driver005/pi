module;

#include <cstdint>

export module pi.support.task_records;

import std;
export import pi.durable.task_definition;
export import pi.types.json;
export import pi.types.ownership_ref;
export import pi.types.task_node;

/** Pure helpers over durable task records (JSON), shared by the scheduler and the harness. */
export class TaskRecords {
public:
    /** The record with a new state; memos disappear once an outcome is decided. */
    Json withState(const Json& record, const Json& state) const {
        Json next = record;
        const std::string status = state.at("status").get<std::string>();
        if (status == "terminal" || status == "completing") {
            next.erase("memos");
        }
        next["state"] = state;
        return next;
    }

    /** Whether the record holds or ends with an outcome other than `completed`. */
    bool failedOutcome(const Json& record) const {
        const Json& state = record.at("state");
        const std::string status = state.at("status").get<std::string>();
        return (status == "completing" || status == "terminal") && state.at("outcome").at("status") != "completed";
    }

    /** A live owner's durable cancellation intent: its abort mark, or a held outcome other than `completed`. */
    bool cancellationIntent(const Json& record) const {
        return record.at("state").at("status") != "terminal" &&
               (record.value("abortRequested", false) || failedOutcome(record));
    }

    TaskNode nodeOf(const Json& record) const {
        TaskNode node;
        node.conversationId = record.at("conversationId").get<std::int64_t>();
        if (record.contains("owner")) {
            node.owner = record.at("owner").get<std::int64_t>();
        }
        node.background = record.value("background", false);
        return node;
    }

    OwnershipRef parentOf(const TaskNode& node) const {
        return node.owner ? OwnershipRef{true, *node.owner} : OwnershipRef{false, node.conversationId};
    }

    OwnershipRef parentOf(const Json& record) const {
        return parentOf(nodeOf(record));
    }

    /** The task's own memo `name`; nothing when unset. */
    std::optional<Json> memoOf(const Json& record, const std::string& name) const {
        if (!record.contains("memos") || !record.at("memos").contains(name)) {
            return std::nullopt;
        }
        return record.at("memos").at(name);
    }

    /** Whether a definition can take the task at reservation: same version, or newer with a migration. */
    bool canReserve(const TaskDefinition& definition, const Json& record) const {
        const std::int64_t version = record.at("version").get<std::int64_t>();
        return definition.version == version || (definition.version > version && static_cast<bool>(definition.migrate));
    }
};
