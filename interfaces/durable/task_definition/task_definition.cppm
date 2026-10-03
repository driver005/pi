module;

#include <cstdint>

export module pi.durable.task_definition;

import std;
export import pi.durable.i_task_runtime;
export import pi.types.json;
export import pi.types.result;

/**
 * An executable durable state machine, registered in the registry by `name`. A phase handler receives the running
 * task record and must commit a changed checkpoint or a terminal outcome through `runtime.commit()`; returning
 * without durable progress faults the task. Port of TaskDefinition in packages/durable/src/types.ts.
 */
export struct TaskDefinition {
    /** Registered task kind persisted in the record's `kind`. */
    std::string name;
    /** Definition version persisted with live input and checkpoints. */
    std::int64_t version = 1;
    /** The first checkpoint (`{phase, ...}`) of a new task. */
    std::function<Json(const Json& input)> initial;
    /** One handler per checkpoint phase. */
    std::map<std::string, std::function<Result<void>(const Json& task, ITaskRuntime& runtime)>> phases;
    /** Runs in a fresh invocation after an abort mark and must commit a terminal outcome. */
    std::function<Result<void>(const Json& task, ITaskRuntime& runtime)> abort;
    /** Converts a record stored by an older version to `{input, checkpoint}`; runs at reservation. */
    std::function<Result<Json>(const Json& input, const Json& checkpoint, std::int64_t fromVersion)> migrate;
};
