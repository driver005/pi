module;

#include <cstdint>

export module pi.support.outcome_settlement;

import std;
export import pi.support.assistant_entries;
export import pi.support.builtin_documents;
export import pi.support.compaction_planner;
export import pi.support.live_editor;
export import pi.support.run_starter;
export import pi.support.tool_task_definition;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;

/**
 * Harness cleanup for a terminal outcome the scheduler writes itself (`faulted` or `orphaned`). A run task ends its run; a
 * tool task's slot is marked done without an entry, and context derivation synthesizes the missing result; a compaction
 * task's status is removed. Other kinds are ignored so `pi.live` is never created elsewhere. A committed generation partial
 * becomes an aborted assistant entry here, exactly as in the generation abort handler, so the transcript keeps what the
 * model produced and `pi.usage` counts its spend. Port of settleSchedulerOutcome() in live.ts.
 */
export class OutcomeSettlement {
public:
    Result<void> settle(Transaction& tx, const Json& record, const Json& outcome) const {
        const std::string kind = record.at("kind").get<std::string>();
        const std::int64_t conversationId = record.at("conversationId").get<std::int64_t>();
        const std::int64_t taskId = record.at("id").get<std::int64_t>();
        if (kind != m_tools.kind() && kind != m_compaction.taskKind() && kind != m_runs.generationKind()) {
            return {};
        }
        DocAddressArgs args;
        args.owner = conversationId;
        auto live = tx.doc(m_documents.live(), args);
        if (!live) {
            return std::unexpected(live.error());
        }
        if (kind == m_tools.kind()) {
            if (Json* slot = m_live.toolSlot(**live, taskId)) {
                m_live.finishSlot(*slot, std::nullopt);
            }
            return {};
        }
        if (kind == m_compaction.taskKind()) {
            m_live.removeCompactionStatus(**live, taskId);
            return {};
        }
        if (!(*live)->contains("run") || (*live)->at("run").at("taskId").get<std::int64_t>() != taskId) {
            return {};
        }
        if (auto converted = m_assistants.convertPartial(tx, **live, conversationId); !converted) {
            return converted;
        }
        const Json settlement = outcome.at("status") == "faulted"
                                    ? Json::object({{"status", "unanswered"}, {"reason", "faulted"}, {"detail", outcome.at("error").at("message")}})
                                    : Json::object({{"status", "unanswered"}, {"reason", outcome.at("reason")}});
        return m_live.endRun(tx, **live, taskId, settlement);
    }

private:
    BuiltinDocuments m_documents;
    LiveEditor m_live;
    AssistantEntries m_assistants;
    RunStarter m_runs;
    CompactionPlanner m_compaction;
    ToolTaskDefinition m_tools;
};
