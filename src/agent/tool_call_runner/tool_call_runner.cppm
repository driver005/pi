export module pi.agent.tool_call_runner;

import std;
export import pi.agent.i_tool_call_runner;
export import pi.support.schema_validator;

/** Standard IToolCallRunner: lookup, validate with SchemaValidator, hooks, execute. */
export class ToolCallRunner : public IToolCallRunner {
public:
    PreparedToolCall prepare(const AgentContext& context, const AssistantMessage& assistant, const ToolCall& call, const AgentLoopConfig& hooks, const std::shared_ptr<AbortSignal>& signal) override {
        std::shared_ptr<ITool> tool;
        for (const auto& candidate : context.tools) {
            if (candidate->definition().name == call.name) {
                tool = candidate;
                break;
            }
        }
        if (tool == nullptr) {
            return immediate(call, "Tool " + call.name + " not found");
        }
        const Json prepared = tool->prepareArguments(call.arguments);
        auto validated = m_validator.validateArguments(tool->definition().parameters, prepared);
        if (!validated.has_value()) {
            return immediate(call, validationMessage(call, validated.error().message));
        }
        Json finalArgs = *validated;
        if (hooks.beforeToolCall) {
            ToolCallContext hookContext;
            hookContext.assistantMessage = &assistant;
            hookContext.toolCall = &call;
            hookContext.args = *validated;
            hookContext.context = &context;
            const auto before = hooks.beforeToolCall(hookContext, signal);
            if (signal != nullptr && signal->aborted()) {
                return immediate(call, "Operation aborted");
            }
            if (before.has_value() && before->block) {
                PreparedToolCall blocked = immediate(call, before->reason.value_or("Tool execution was blocked"));
                blocked.immediateResult.terminate = before->terminate;
                return blocked;
            }
            if (before.has_value() && !before->args.is_null()) {
                finalArgs = before->args;
            }
        }
        if (signal != nullptr && signal->aborted()) {
            return immediate(call, "Operation aborted");
        }
        PreparedToolCall ready;
        ready.toolCall = call;
        ready.tool = std::move(tool);
        ready.args = std::move(finalArgs);
        return ready;
    }

    ToolCallOutcome execute(const AgentContext& context, const AssistantMessage& assistant, const PreparedToolCall& prepared, const AgentLoopConfig& hooks, const std::shared_ptr<AbortSignal>& signal, const ToolUpdateCallback& onUpdate) override {
        ToolCallOutcome outcome;
        outcome.toolCall = prepared.toolCall;
        if (prepared.immediate) {
            outcome.result = prepared.immediateResult;
            outcome.isError = prepared.immediateIsError;
            return outcome;
        }
        bool isError = false;
        outcome.result = runTool(prepared, signal, onUpdate, isError);
        outcome.isError = isError;
        applyAfterHook(context, assistant, prepared, hooks, signal, outcome);
        return outcome;
    }

    ToolCallOutcome run(const AgentContext& context, const AssistantMessage& assistant, const ToolCall& call, const AgentLoopConfig& hooks, const std::shared_ptr<AbortSignal>& signal, const ToolUpdateCallback& onUpdate) override {
        const PreparedToolCall prepared = prepare(context, assistant, call, hooks, signal);
        return execute(context, assistant, prepared, hooks, signal, onUpdate);
    }

private:
    PreparedToolCall immediate(const ToolCall& call, const std::string& message) const {
        PreparedToolCall prepared;
        prepared.toolCall = call;
        prepared.immediate = true;
        prepared.immediateResult = errorResult(message);
        prepared.immediateIsError = true;
        return prepared;
    }

    AgentToolResult errorResult(const std::string& message) const {
        AgentToolResult result;
        result.content.emplace_back(TextContent{message, std::nullopt});
        return result;
    }

    std::string validationMessage(const ToolCall& call, const std::string& issues) const {
        return "Validation failed for tool \"" + call.name + "\":\n" + issues +
               "\n\nReceived arguments:\n" + call.arguments.dump(2);
    }

    AgentToolResult runTool(const PreparedToolCall& prepared, const std::shared_ptr<AbortSignal>& signal, const ToolUpdateCallback& onUpdate, bool& isError) const {
        auto accepting = std::make_shared<std::atomic<bool>>(true);
        const ToolUpdateCallback guarded = [accepting, onUpdate](const AgentToolResult& partial) {
            if (accepting->load() && onUpdate) {
                onUpdate(partial);
            }
        };
        auto outcome = prepared.tool->execute(prepared.toolCall.id, prepared.args, signal, guarded);
        accepting->store(false);
        if (!outcome.has_value()) {
            isError = true;
            return errorResult(outcome.error().message);
        }
        isError = outcome->isError;
        return std::move(*outcome);
    }

    void applyAfterHook(const AgentContext& context, const AssistantMessage& assistant, const PreparedToolCall& prepared, const AgentLoopConfig& hooks, const std::shared_ptr<AbortSignal>& signal, ToolCallOutcome& outcome) const {
        if (!hooks.afterToolCall) {
            return;
        }
        ToolCallContext hookContext;
        hookContext.assistantMessage = &assistant;
        hookContext.toolCall = &prepared.toolCall;
        hookContext.args = prepared.args;
        hookContext.context = &context;
        hookContext.result = outcome.result;
        hookContext.isError = outcome.isError;
        const auto override = hooks.afterToolCall(hookContext, signal);
        if (override.has_value()) {
            mergeOverride(*override, outcome);
        }
    }

    void mergeOverride(const AfterToolCallResult& override, ToolCallOutcome& outcome) const {
        AgentToolResult& result = outcome.result;
        const bool structuredReplaced = !override.structuredContent.is_null();
        if (override.content.has_value()) {
            result.content = *override.content;
        }
        if (!override.details.is_null()) {
            result.details = override.details;
        }
        if (override.usage.has_value()) {
            result.usage = override.usage;
        }
        if (override.terminate.has_value()) {
            result.terminate = *override.terminate;
        }
        if (structuredReplaced) {
            result.structuredContent = override.structuredContent;
        } else if (override.content.has_value()) {
            result.structuredContent = Json();
        }
        if (override.isError.has_value()) {
            outcome.isError = *override.isError;
        }
    }

    SchemaValidator m_validator;
};
