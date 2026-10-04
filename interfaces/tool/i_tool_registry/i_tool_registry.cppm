export module pi.tool.i_tool_registry;

import std;
export import pi.tool.i_tool;

/** Named collection of tools with an active subset (what is offered to the model). Thread-safe. */
export class IToolRegistry {
public:
    virtual ~IToolRegistry() = default;

    /** Adds or replaces the tool of the same name. New tools start active. */
    virtual void add(std::shared_ptr<ITool> tool) = 0;
    virtual bool remove(const std::string& name) = 0;
    virtual std::shared_ptr<ITool> find(const std::string& name) const = 0;
    /** All registered tools in registration order. */
    virtual std::vector<std::shared_ptr<ITool>> all() const = 0;
    /** Tools currently offered to the model, in registration order. */
    virtual std::vector<std::shared_ptr<ITool>> active() const = 0;
    /** Activates exactly the named tools; unknown names are ignored. Returns the active count. */
    virtual std::size_t setActive(const std::vector<std::string>& names) = 0;
};
