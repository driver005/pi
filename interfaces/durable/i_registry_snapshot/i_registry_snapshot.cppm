export module pi.durable.i_registry_snapshot;

import std;
export import pi.durable.task_definition;
export import pi.types.extension;

/** Immutable view of one published registry state. */
export class IRegistrySnapshot {
public:
    virtual ~IRegistrySnapshot() = default;

    virtual std::vector<std::shared_ptr<const Extension>> installed() const = 0;
    virtual std::shared_ptr<const Extension> extension(const std::string& name) const = 0;
    /** Built-in and installed task definitions. */
    virtual std::vector<std::shared_ptr<const TaskDefinition>> tasks() const = 0;
    virtual std::shared_ptr<const TaskDefinition> task(const std::string& name) const = 0;
};
