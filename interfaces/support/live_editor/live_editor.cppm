module;

#include <cstdint>

export module pi.support.live_editor;

import std;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;

/**
 * Edits of the `pi.live` document of a conversation (run control, the current tool round, compaction statuses).
 * Port of the helpers in packages/durable/src/harness/live.ts.
 */
export class LiveEditor {
public:
    /**
     * Ends the run owned by `taskId`: settles each of its inputs with `settlement` and removes `run`. Always removes
     * `generation` and `tools`, whose presentation belongs to the ending run.
     */
    Result<void> endRun(Transaction& tx, Json& live, std::int64_t taskId, const Json& settlement) const {
        if (live.contains("run") && live.at("run").at("taskId").get<std::int64_t>() == taskId) {
            for (const Json& id : live.at("run").at("inputs")) {
                if (auto settled = tx.settleSubmission(id.get<std::int64_t>(), settlement); !settled) {
                    return settled;
                }
            }
            live.erase("run");
        }
        live.erase("generation");
        live.erase("tools");
        return {};
    }

    /** Adds the status of a compaction task created in this commit; statuses stay in task id order. */
    void addCompactionStatus(Json& live, const Json& status) const {
        if (!live.contains("compactions")) {
            live["compactions"] = Json::array();
        }
        live["compactions"].push_back(status);
    }

    /** The status of compaction task `taskId`, if listed. */
    Json* compactionStatus(Json& live, std::int64_t taskId) const {
        return find(live, "compactions", taskId);
    }

    /** Removes the status of compaction task `taskId`, and the list once empty. */
    void removeCompactionStatus(Json& live, std::int64_t taskId) const {
        if (!live.contains("compactions")) {
            return;
        }
        Json& statuses = live["compactions"];
        for (std::size_t i = 0; i < statuses.size(); ++i) {
            if (statuses[i].at("taskId").get<std::int64_t>() == taskId) {
                statuses.erase(i);
                break;
            }
        }
        if (statuses.empty()) {
            live.erase("compactions");
        }
    }

    /** The slot of tool task `taskId` in the current round, if the round still lists it. */
    Json* toolSlot(Json& live, std::int64_t taskId) const {
        return find(live, "tools", taskId);
    }

    /** Marks a slot done: the result entry, if any, now carries its running output, details and diagnostics. */
    void finishSlot(Json& slot, const std::optional<std::int64_t>& entry) const {
        slot["status"] = "done";
        if (entry) {
            slot["entry"] = *entry;
        }
        clearProgress(slot);
    }

    /** Removes what a tool published while running; its result entry or a rerun replaces it. */
    void clearProgress(Json& slot) const {
        for (const char* key : {"output", "droppedBytes", "droppedLines", "details", "diagnostics"}) {
            slot.erase(key);
        }
    }

private:
    Json* find(Json& live, const char* list, std::int64_t taskId) const {
        if (!live.contains(list)) {
            return nullptr;
        }
        for (Json& item : live[list]) {
            if (item.contains("taskId") && item.at("taskId").get<std::int64_t>() == taskId) {
                return &item;
            }
        }
        return nullptr;
    }
};
