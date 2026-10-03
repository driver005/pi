export module pi.types.task_reservation;

import std;
export import pi.durable.i_registry_snapshot;
export import pi.durable.task_definition;
export import pi.types.task_invocation;

/** An invocation the scheduler reserved, with the definition and registry snapshot it will run under. */
export struct TaskReservation {
    std::shared_ptr<TaskInvocation> invocation;
    std::shared_ptr<const TaskDefinition> definition;
    std::shared_ptr<const IRegistrySnapshot> snapshot;
};
