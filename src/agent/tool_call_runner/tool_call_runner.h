#pragma once

#include <memory>
#include <string>

#include "interfaces/agent/i_tool_call_runner/i_tool_call_runner.h"
#include "interfaces/support/schema_validator/schema_validator.h"

/** Standard IToolCallRunner: lookup, validate with SchemaValidator, hooks, execute. */
class ToolCallRunner : public IToolCallRunner {
public:
    PreparedToolCall prepare(const AgentContext& context, const AssistantMessage& assistant,
                             const ToolCall& call, const AgentLoopConfig& hooks,
                             const std::shared_ptr<AbortSignal>& signal) override;

    ToolCallOutcome execute(const AgentContext& context, const AssistantMessage& assistant,
                            const PreparedToolCall& prepared, const AgentLoopConfig& hooks,
                            const std::shared_ptr<AbortSignal>& signal,
                            const ToolUpdateCallback& onUpdate) override;

    ToolCallOutcome run(const AgentContext& context, const AssistantMessage& assistant,
                        const ToolCall& call, const AgentLoopConfig& hooks,
                        const std::shared_ptr<AbortSignal>& signal,
                        const ToolUpdateCallback& onUpdate) override;

private:
    PreparedToolCall immediate(const ToolCall& call, const std::string& message) const;
    AgentToolResult errorResult(const std::string& message) const;
    std::string validationMessage(const ToolCall& call, const std::string& issues) const;
    AgentToolResult runTool(const PreparedToolCall& prepared,
                            const std::shared_ptr<AbortSignal>& signal,
                            const ToolUpdateCallback& onUpdate, bool& isError) const;
    void applyAfterHook(const AgentContext& context, const AssistantMessage& assistant,
                        const PreparedToolCall& prepared, const AgentLoopConfig& hooks,
                        const std::shared_ptr<AbortSignal>& signal, ToolCallOutcome& outcome) const;
    void mergeOverride(const AfterToolCallResult& override, ToolCallOutcome& outcome) const;

    SchemaValidator m_validator;
};
