module;

#include <cstdint>

export module pi.types.task_inspection;

import std;
export import pi.types.json;

/** A live task and what the scheduler would do with it under the current registry. */
export struct TaskInspection {
    Json record;
    /** "running", "ready", "waiting", "completing" or "blocked". */
    std::string state;
    /** ready: its definition is newer and has `migrate`. */
    bool migrates = false;
    /** waiting: the live tasks it waits for. */
    std::vector<std::int64_t> on;
    /** blocked: "missing_task", "task_too_old" or "migration_failed". */
    std::string reason;
    std::optional<std::string> error;
};
