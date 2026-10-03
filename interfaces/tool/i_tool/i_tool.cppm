export module pi.tool.i_tool;

import std;
export import pi.support.abort_signal;
export import pi.types.agent_tool_result;
export import pi.types.json;
export import pi.types.result;
export import pi.types.tool;
export import pi.types.tool_execution_mode;

/** Callback a running tool uses to stream partial results. Ignored after execute returns. */
export using ToolUpdateCallback = std::function<void(const AgentToolResult&)>;

/** A tool the agent can execute. Implementations must be thread-safe (parallel execution). */
export class ITool {
public:
    virtual ~ITool() = default;

    /** Declaration shown to the model (name, description, JSON Schema parameters). */
    virtual const Tool& definition() const = 0;

    /** Human-readable label for UIs. */
    virtual std::string label() const = 0;

    /** One-line description for the system prompt's tool list; empty leaves the tool unlisted. */
    virtual std::string promptSnippet() const = 0;
    /** Usage rules added to the system prompt while the tool is active. */
    virtual std::vector<std::string> promptGuidelines() const = 0;
    /** Per-tool override of the execution mode; nullopt uses the agent default. */
    virtual std::optional<ToolExecutionMode> executionMode() const = 0;

    /** Compatibility shim applied to raw model arguments before schema validation. */
    virtual Json prepareArguments(const Json& arguments) const = 0;

    /**
     * Runs the call with validated arguments. Return an Error for failures; the model then sees
     * the error text. A result with isError set reports a failure while keeping its details.
     */
    virtual Result<AgentToolResult> execute(const std::string& toolCallId, const Json& params,
                                            const std::shared_ptr<AbortSignal>& signal,
                                            const ToolUpdateCallback& onUpdate) = 0;
};
