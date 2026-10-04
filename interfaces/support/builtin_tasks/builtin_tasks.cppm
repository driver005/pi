export module pi.support.builtin_tasks;

import std;
export import pi.durable.task_definition;
export import pi.support.compaction_task_definition;
export import pi.support.generation_task_definition;
export import pi.support.tool_task_definition;

/** The built-in task definitions every registry holds: generation, tool and compaction. */
export class BuiltinTasks {
public:
    std::vector<std::shared_ptr<const TaskDefinition>> all() const {
        return {GenerationTaskDefinition().build(), ToolTaskDefinition().build(), CompactionTaskDefinition().build()};
    }

    std::vector<std::string> kinds() const {
        return {"pi.generation", "pi.tool", "pi.compaction"};
    }
};
