module;

#include <cstdint>

export module pi.support.run_starter;

import std;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;
export import pi.types.task_options;

/**
 * Starts runs and hands them over. A run is a generation task owned by its conversation; `pi.live.run` records the
 * task that settles the run's inputs. Port of startRun/createGeneration/handOver in generation.ts, kept apart from the
 * generation definition so submissions and compaction can start runs without depending on it.
 */
export class RunStarter {
public:
    /** The generation task kind and the first checkpoint of a new generation. */
    std::string generationKind() const {
        return "pi.generation";
    }

    Json generationCheckpoint(std::int64_t attempt = 1) const {
        return Json::object({{"phase", "prepare"}, {"attempt", attempt}});
    }

    /** A generation owned by its conversation. */
    Result<std::int64_t> createGeneration(Transaction& tx, std::int64_t conversationId) const {
        TaskOptions options;
        options.ownership = Json::object({{"kind", "conversation"}});
        options.conversationId = conversationId;
        return tx.createTask(generationKind(), 1, Json::object(), generationCheckpoint(), options);
    }

    /** Starts a run for `inputs`, placed input submissions: a new generation takes `pi.live.run`. */
    Result<void> startRun(Transaction& tx, std::int64_t conversationId, Json& live, const std::vector<std::int64_t>& inputs) const {
        auto task = createGeneration(tx, conversationId);
        if (!task) {
            return std::unexpected(task.error());
        }
        live["run"] = Json::object({{"taskId", *task}, {"inputs", Json(inputs)}});
        return {};
    }

    /** Hands run control from `from` to `to`; the run's inputs move with it. */
    void handOver(Json& live, std::int64_t from, std::int64_t to) const {
        if (live.contains("run") && live.at("run").at("taskId").get<std::int64_t>() == from) {
            live["run"]["taskId"] = to;
        }
    }
};
