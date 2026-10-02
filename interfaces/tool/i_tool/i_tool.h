#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/types/agent_tool_result/agent_tool_result.h"
#include "interfaces/types/json/json.h"
#include "interfaces/types/result/result.h"
#include "interfaces/types/tool/tool.h"
#include "interfaces/types/tool_execution_mode/tool_execution_mode.h"

/** Callback a running tool uses to stream partial results. Ignored after execute returns. */
using ToolUpdateCallback = std::function<void(const AgentToolResult&)>;

/** A tool the agent can execute. Implementations must be thread-safe (parallel execution). */
class ITool {
public:
    virtual ~ITool() = default;

    /** Declaration shown to the model (name, description, JSON Schema parameters). */
    virtual const Tool& definition() const = 0;

    /** Human-readable label for UIs. */
    virtual std::string label() const = 0;

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
