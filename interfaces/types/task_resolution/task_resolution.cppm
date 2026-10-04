export module pi.types.task_resolution;

import std;
export import pi.durable.task_definition;
export import pi.types.json;

/** A definition that can take a record, or why none can; deciding it runs no task code. */
export struct TaskResolution {
    bool ready = false;
    std::shared_ptr<const TaskDefinition> definition;
    /** The record, migrated when `migrates`. */
    Json record;
    bool migrates = false;
    /** blocked: "missing_task", "task_too_old" or "migration_failed". */
    std::string reason;
    std::optional<std::string> error;
};
