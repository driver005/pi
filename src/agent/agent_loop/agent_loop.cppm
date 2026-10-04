export module pi.agent.agent_loop;

import std;
export import pi.agent.i_agent_loop;
export import pi.agent.i_tool_call_runner;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.support.transcript_normalizer;
export import pi.types.agent_run_state;
export import pi.types.tool_batch_result;
export import pi.types.tool_call_outcome;

/**
 * Port of packages/agent/src/agent-loop.ts. Works with AgentMessage throughout and converts to
 * LLM messages only at the provider boundary. Tool calls of one assistant message run through
 * the injected IToolCallRunner, concurrently on the executor unless sequential mode applies.
 */
export class AgentLoop : public IAgentLoop {
public:
    AgentLoop(IToolCallRunner& toolCalls, IExecutor& executor, const IClock& clock)
        : m_toolCalls(toolCalls),
          m_executor(executor),
          m_clock(clock) {}

    std::vector<AgentMessage> run(std::vector<AgentMessage> prompts, AgentContext context, const AgentLoopConfig& config, const AgentEventSink& emit, const std::shared_ptr<AbortSignal>& signal, const StreamFn& streamFn) override {
        AgentRunState state;
        state.config = config;
        state.signal = signal;
        state.emit = emit;
        state.streamFn = streamFn;
        state.context = std::move(context);
        std::vector<AgentMessage> initial = declareToolChanges(state.context, std::move(prompts));
        state.newMessages = initial;
        state.context.messages.insert(state.context.messages.end(), initial.begin(), initial.end());
        emitSignal(state, AgentEventType::AgentStart);
        emitSignal(state, AgentEventType::TurnStart);
        for (const AgentMessage& message : initial) {
            emitMessagePair(state, message);
        }
        runLoop(state);
        return state.newMessages;
    }

    Result<std::vector<AgentMessage>> runContinue(AgentContext context, const AgentLoopConfig& config, const AgentEventSink& emit, const std::shared_ptr<AbortSignal>& signal, const StreamFn& streamFn) override {
        if (context.messages.empty()) {
            return std::unexpected(Error{"invalid_continue", "Cannot continue: no messages in context"});
        }
        if (std::holds_alternative<AssistantMessage>(context.messages.back())) {
            return std::unexpected(Error{"invalid_continue", "Cannot continue from message role: assistant"});
        }
        AgentRunState state;
        state.config = config;
        state.signal = signal;
        state.emit = emit;
        state.streamFn = streamFn;
        state.context = std::move(context);
        emitSignal(state, AgentEventType::AgentStart);
        emitSignal(state, AgentEventType::TurnStart);
        runLoop(state);
        return state.newMessages;
    }

private:
    void emitEvent(AgentRunState& state, AgentEvent event) const {
        const std::lock_guard<std::mutex> lock(state.emitMutex);
        if (state.emit) {
            state.emit(event);
        }
    }

    void emitSignal(AgentRunState& state, AgentEventType type) const {
        AgentEvent event;
        event.type = type;
        emitEvent(state, std::move(event));
    }

    void emitMessagePair(AgentRunState& state, const AgentMessage& message) const {
        AgentEvent start;
        start.type = AgentEventType::MessageStart;
        start.message = std::make_shared<const AgentMessage>(message);
        emitEvent(state, start);
        AgentEvent end;
        end.type = AgentEventType::MessageEnd;
        end.message = start.message;
        emitEvent(state, end);
    }

    void emitTurnEnd(AgentRunState& state, const AssistantMessage& message, const std::vector<ToolResultMessage>& toolResults) const {
        AgentEvent event;
        event.type = AgentEventType::TurnEnd;
        event.message = std::make_shared<const AgentMessage>(message);
        event.toolResults = toolResults;
        emitEvent(state, event);
    }

    void runLoop(AgentRunState& state) const {
        std::vector<AgentMessage> pending = pollSteering(state);
        while (true) {
            bool hasMoreToolCalls = true;
            while (hasMoreToolCalls || !pending.empty()) {
                if (runTurn(state, pending, hasMoreToolCalls)) {
                    return;
                }
            }
            std::vector<AgentMessage> followUp =
                state.config.getFollowUpMessages ? state.config.getFollowUpMessages()
                                                 : std::vector<AgentMessage>{};
            if (!followUp.empty()) {
                state.explicitContinuation = false;
                pending = std::move(followUp);
                continue;
            }
            if (state.explicitContinuation) {
                state.explicitContinuation = false;
                continue;
            }
            break;
        }
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        end.messages = state.newMessages;
        emitEvent(state, end);
    }

    bool runTurn(AgentRunState& state, std::vector<AgentMessage>& pending, bool& hasMoreToolCalls) const {
        std::vector<AgentMessage> prepared;
        startTurn(state, pending, prepared);
        prepared.insert(prepared.end(), pending.begin(), pending.end());
        for (const AgentMessage& message : declareToolChanges(state.context, std::move(prepared))) {
            emitMessagePair(state, message);
            state.context.messages.push_back(message);
            state.newMessages.push_back(message);
        }
        pending.clear();
        if (state.config.prepareRequest) {
            PrepareRequestContext request;
            request.context = &state.context;
            request.model = &state.config.model;
            request.thinkingLevel = state.config.options.reasoning;
            const auto update = state.config.prepareRequest(request, state.signal);
            applyRequestUpdate(state, update);
        }
        AssistantMessage message = streamAssistantResponse(state);
        state.newMessages.push_back(message);
        const bool failed = message.stopReason == StopReason::Error || message.stopReason == StopReason::Aborted;
        std::vector<ToolResultMessage> toolResults;
        hasMoreToolCalls = false;
        if (!failed) {
            const std::vector<ToolCall> calls = toolCallsOf(message);
            if (!calls.empty()) {
                ToolBatchResult batch = message.stopReason == StopReason::Length
                                            ? failTruncatedToolCalls(state, calls)
                                            : executeToolCalls(state, message);
                hasMoreToolCalls = !batch.terminate;
                for (ToolResultMessage& result : batch.messages) {
                    state.context.messages.emplace_back(result);
                    state.newMessages.emplace_back(result);
                    toolResults.push_back(std::move(result));
                }
            }
        }
        state.lastCompletedTurn = AgentTurnContext{message, toolResults, state.context, state.newMessages};
        std::optional<AgentTurnAction> decision;
        if (state.config.finishTurn) {
            decision = state.config.finishTurn(*state.lastCompletedTurn, state.signal);
        }
        emitTurnEnd(state, message, toolResults);
        if (failed || decision == AgentTurnAction::End) {
            AgentEvent end;
            end.type = AgentEventType::AgentEnd;
            end.messages = state.newMessages;
            emitEvent(state, end);
            return true;
        }
        state.explicitContinuation = decision == AgentTurnAction::Continue;
        pending = pollSteering(state);
        if (hasMoreToolCalls || !pending.empty()) {
            state.explicitContinuation = false;
        }
        return false;
    }

    void startTurn(AgentRunState& state, std::vector<AgentMessage>& pending, std::vector<AgentMessage>& prepared) const {
        if (!state.lastCompletedTurn.has_value()) {
            return;
        }
        if (state.config.prepareNextTurn) {
            const auto update = state.config.prepareNextTurn(*state.lastCompletedTurn);
            applyUpdate(state, update);
            if (update.has_value()) {
                prepared = update->messages;
            }
        }
        if (pending.empty()) {
            pending = pollSteering(state);
        }
        emitSignal(state, AgentEventType::TurnStart);
    }

    void applyUpdate(AgentRunState& state, const std::optional<AgentLoopTurnUpdate>& update) const {
        if (!update.has_value()) {
            return;
        }
        if (update->context.has_value()) {
            state.context = *update->context;
        }
        if (update->model.has_value()) {
            state.config.model = *update->model;
        }
        if (update->thinkingLevel.has_value()) {
            state.config.options.reasoning = *update->thinkingLevel;
        }
    }

    void applyRequestUpdate(AgentRunState& state, const std::optional<AgentLoopTurnUpdate>& update) const {
        applyUpdate(state, update);
    }
    void appendPending(AgentRunState& state, std::vector<AgentMessage> messages) const;

    std::vector<AgentMessage> pollSteering(const AgentRunState& state) const {
        return state.config.getSteeringMessages ? state.config.getSteeringMessages()
                                                : std::vector<AgentMessage>{};
    }

    std::vector<AgentMessage> declareToolChanges(const AgentContext& context, std::vector<AgentMessage> pending) const {
        int systemIndex = -1;
        for (int i = static_cast<int>(pending.size()) - 1; i >= 0; --i) {
            if (std::holds_alternative<SystemMessage>(pending[static_cast<std::size_t>(i)])) {
                systemIndex = i;
                break;
            }
        }
        std::vector<AgentMessage> baseline = pending;
        if (systemIndex >= 0) {
            auto& system = std::get<SystemMessage>(baseline[static_cast<std::size_t>(systemIndex)]);
            system = withToolChanges(system, ToolStateChanges{});
        }
        std::vector<AgentMessage> visible = context.messages;
        visible.insert(visible.end(), baseline.begin(), baseline.end());
        std::vector<Tool> declared;
        for (const auto& tool : context.tools) {
            declared.push_back(m_transcript.toToolDeclaration(tool->definition()));
        }
        const ToolStateChanges changes =
            m_transcript.toolStateChanges(m_transcript.currentTools(llmMessages(visible)), declared);
        const bool unchanged = changes.toolsAdded.empty() && changes.toolsRemoved.empty();
        if (systemIndex >= 0) {
            const auto& original = std::get<SystemMessage>(pending[static_cast<std::size_t>(systemIndex)]);
            const bool declaresNone = (!original.toolsAdded.has_value() || original.toolsAdded->empty()) &&
                                      (!original.toolsRemoved.has_value() || original.toolsRemoved->empty());
            if (unchanged && declaresNone) {
                return pending;
            }
            baseline[static_cast<std::size_t>(systemIndex)] = withToolChanges(original, changes);
            return baseline;
        }
        if (unchanged) {
            return pending;
        }
        SystemMessage update;
        update.content = std::string();
        update.timestamp = m_clock.nowMs();
        update = withToolChanges(update, changes);
        std::size_t insertAt = pending.size();
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!std::holds_alternative<SystemMessage>(pending[i])) {
                insertAt = i;
                break;
            }
        }
        pending.insert(pending.begin() + static_cast<std::ptrdiff_t>(insertAt), AgentMessage(std::move(update)));
        return pending;
    }

    SystemMessage withToolChanges(const SystemMessage& message, const ToolStateChanges& changes) const {
        SystemMessage copy = message;
        copy.toolsAdded.reset();
        copy.toolsRemoved.reset();
        if (!changes.toolsAdded.empty()) {
            copy.toolsAdded = changes.toolsAdded;
        }
        if (!changes.toolsRemoved.empty()) {
            copy.toolsRemoved = changes.toolsRemoved;
        }
        return copy;
    }

    std::vector<Message> llmMessages(const std::vector<AgentMessage>& messages) const {
        std::vector<Message> out;
        for (const AgentMessage& message : messages) {
            std::visit(
                [&out](const auto& value) {
                    using Type = std::decay_t<decltype(value)>;
                    if constexpr (!std::is_same_v<Type, CustomMessage>) {
                        out.emplace_back(value);
                    }
                },
                message);
        }
        return out;
    }

    AssistantMessage streamAssistantResponse(AgentRunState& state) const {
        std::vector<AgentMessage> messages = state.context.messages;
        if (state.config.transformContext) {
            messages = state.config.transformContext(messages, state.signal);
        }
        Context context;
        context.messages = state.config.convertToLlm ? state.config.convertToLlm(messages) : llmMessages(messages);
        const TranscriptContext transcript = m_transcript.normalizeContext(context);
        StreamOptions options = state.config.options;
        if (state.config.getApiKey) {
            if (auto key = state.config.getApiKey(state.config.model.provider); key.has_value() && !key->empty()) {
                options.apiKey = std::move(key);
            }
        }
        options.signal = state.signal;
        const std::shared_ptr<AssistantMessageStream> stream =
            state.streamFn(state.config.model, transcript, options);
        bool addedPartial = false;
        while (auto event = stream->next()) {
            if (event->type == AssistantEventType::Done || event->type == AssistantEventType::Error) {
                break;
            }
            handleStreamEvent(state, *event, addedPartial);
        }
        AssistantMessage message = finalMessage(state, *stream);
        if (addedPartial) {
            state.context.messages.back() = message;
        } else {
            state.context.messages.emplace_back(message);
            AgentEvent started;
            started.type = AgentEventType::MessageStart;
            started.message = std::make_shared<const AgentMessage>(message);
            emitEvent(state, started);
        }
        AgentEvent ended;
        ended.type = AgentEventType::MessageEnd;
        ended.message = std::make_shared<const AgentMessage>(message);
        emitEvent(state, ended);
        return message;
    }

    AssistantMessage finalMessage(AgentRunState& state, AssistantMessageStream& stream) const {
        std::optional<AssistantMessage> result = stream.result();
        AssistantMessage message =
            result.has_value() ? std::move(*result) : syntheticError(state, "Provider stream ended without a result");
        message.thinkingLevel = state.config.options.reasoning;
        return message;
    }

    void handleStreamEvent(AgentRunState& state, const AssistantMessageEvent& event, bool& addedPartial) const {
        if (event.type == AssistantEventType::Start) {
            state.context.messages.emplace_back(*event.partial);
            addedPartial = true;
            AgentEvent started;
            started.type = AgentEventType::MessageStart;
            started.message = std::make_shared<const AgentMessage>(*event.partial);
            emitEvent(state, started);
            return;
        }
        if (!addedPartial || event.partial == nullptr) {
            return;
        }
        state.context.messages.back() = *event.partial;
        AgentEvent update;
        update.type = AgentEventType::MessageUpdate;
        update.message = std::make_shared<const AgentMessage>(*event.partial);
        update.assistantEvent = std::make_shared<const AssistantMessageEvent>(event);
        emitEvent(state, update);
    }

    ToolBatchResult executeToolCalls(AgentRunState& state, const AssistantMessage& assistant) const {
        const std::vector<ToolCall> calls = toolCallsOf(assistant);
        return wantsSequential(state, calls) ? executeSequential(state, assistant, calls)
                                             : executeParallel(state, assistant, calls);
    }

    ToolBatchResult failTruncatedToolCalls(AgentRunState& state, const std::vector<ToolCall>& calls) const {
        ToolBatchResult batch;
        for (const ToolCall& call : calls) {
            emitToolStart(state, call);
            const ToolCallOutcome outcome = errorOutcome(
                call, "Tool call \"" + call.name +
                          "\" was not executed: the response hit the output token limit, so its arguments "
                          "may be truncated. Re-issue the tool call with complete arguments.");
            emitToolEnd(state, outcome);
            const ToolResultMessage message = toResultMessage(outcome);
            emitMessagePair(state, message);
            batch.messages.push_back(message);
        }
        return batch;
    }

    ToolBatchResult executeSequential(AgentRunState& state, const AssistantMessage& assistant, const std::vector<ToolCall>& calls) const {
        ToolBatchResult batch;
        std::vector<ToolCallOutcome> outcomes;
        for (const ToolCall& call : calls) {
            emitToolStart(state, call);
            const PreparedToolCall prepared =
                m_toolCalls.prepare(state.context, assistant, call, state.config, state.signal);
            ToolCallOutcome outcome =
                m_toolCalls.execute(state.context, assistant, prepared, state.config, state.signal,
                                    updateEmitter(state, call));
            emitToolEnd(state, outcome);
            const ToolResultMessage message = toResultMessage(outcome);
            emitMessagePair(state, message);
            batch.messages.push_back(message);
            outcomes.push_back(std::move(outcome));
            if (state.signal != nullptr && state.signal->aborted()) {
                break;
            }
        }
        batch.terminate = shouldTerminate(outcomes);
        return batch;
    }

    ToolBatchResult executeParallel(AgentRunState& state, const AssistantMessage& assistant, const std::vector<ToolCall>& calls) const {
        std::vector<PreparedToolCall> prepared;
        std::vector<ToolCallOutcome> outcomes;
        std::vector<bool> settled;
        for (const ToolCall& call : calls) {
            emitToolStart(state, call);
            PreparedToolCall entry = m_toolCalls.prepare(state.context, assistant, call, state.config, state.signal);
            const bool isImmediate = entry.immediate;
            ToolCallOutcome outcome;
            if (isImmediate) {
                outcome = m_toolCalls.execute(state.context, assistant, entry, state.config, state.signal, nullptr);
                emitToolEnd(state, outcome);
            }
            prepared.push_back(std::move(entry));
            outcomes.push_back(std::move(outcome));
            settled.push_back(isImmediate);
            if (state.signal != nullptr && state.signal->aborted()) {
                break;
            }
        }
        std::size_t pendingCount = 0;
        for (const bool done : settled) {
            pendingCount += done ? 0 : 1;
        }
        std::latch finished(static_cast<std::ptrdiff_t>(pendingCount));
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            if (settled[i]) {
                continue;
            }
            m_executor.submit([this, &state, &assistant, &prepared, &outcomes, &finished, i] {
                if (state.signal != nullptr && state.signal->aborted()) {
                    outcomes[i] = errorOutcome(prepared[i].toolCall, "Operation aborted");
                } else {
                    outcomes[i] = m_toolCalls.execute(state.context, assistant, prepared[i], state.config,
                                                      state.signal, updateEmitter(state, prepared[i].toolCall));
                }
                emitToolEnd(state, outcomes[i]);
                finished.count_down();
            });
        }
        finished.wait();
        ToolBatchResult batch;
        for (const ToolCallOutcome& outcome : outcomes) {
            const ToolResultMessage message = toResultMessage(outcome);
            emitMessagePair(state, message);
            batch.messages.push_back(message);
        }
        batch.terminate = shouldTerminate(outcomes);
        return batch;
    }

    void emitToolStart(AgentRunState& state, const ToolCall& call) const {
        AgentEvent event;
        event.type = AgentEventType::ToolExecutionStart;
        event.toolCallId = call.id;
        event.toolName = call.name;
        event.args = call.arguments;
        emitEvent(state, event);
    }

    void emitToolEnd(AgentRunState& state, const ToolCallOutcome& outcome) const {
        AgentEvent event;
        event.type = AgentEventType::ToolExecutionEnd;
        event.toolCallId = outcome.toolCall.id;
        event.toolName = outcome.toolCall.name;
        event.result = std::make_shared<const AgentToolResult>(outcome.result);
        event.isError = outcome.isError;
        emitEvent(state, event);
    }

    ToolUpdateCallback updateEmitter(AgentRunState& state, const ToolCall& call) const {
        return [this, &state, call](const AgentToolResult& partial) {
            AgentEvent event;
            event.type = AgentEventType::ToolExecutionUpdate;
            event.toolCallId = call.id;
            event.toolName = call.name;
            event.args = call.arguments;
            event.result = std::make_shared<const AgentToolResult>(partial);
            emitEvent(state, event);
        };
    }

    ToolResultMessage toResultMessage(const ToolCallOutcome& outcome) const {
        ToolResultMessage message;
        message.toolCallId = outcome.toolCall.id;
        message.toolName = outcome.toolCall.name;
        message.content = outcome.result.content;
        message.details = outcome.result.details;
        message.usage = outcome.result.usage;
        message.isError = outcome.isError;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    ToolCallOutcome errorOutcome(const ToolCall& call, const std::string& message) const {
        ToolCallOutcome outcome;
        outcome.toolCall = call;
        outcome.result.content.emplace_back(TextContent{message, std::nullopt});
        outcome.isError = true;
        return outcome;
    }

    bool shouldTerminate(const std::vector<ToolCallOutcome>& outcomes) const {
        if (outcomes.empty()) {
            return false;
        }
        for (const ToolCallOutcome& outcome : outcomes) {
            if (!outcome.result.terminate) {
                return false;
            }
        }
        return true;
    }

    std::vector<ToolCall> toolCallsOf(const AssistantMessage& message) const {
        std::vector<ToolCall> calls;
        for (const AssistantContentBlock& block : message.content) {
            if (const auto* call = std::get_if<ToolCall>(&block)) {
                calls.push_back(*call);
            }
        }
        return calls;
    }

    bool wantsSequential(const AgentRunState& state, const std::vector<ToolCall>& calls) const {
        if (state.config.toolExecution == ToolExecutionMode::Sequential) {
            return true;
        }
        for (const ToolCall& call : calls) {
            for (const auto& tool : state.context.tools) {
                if (tool->definition().name == call.name &&
                    tool->executionMode() == std::optional<ToolExecutionMode>(ToolExecutionMode::Sequential)) {
                    return true;
                }
            }
        }
        return false;
    }

    AssistantMessage syntheticError(const AgentRunState& state, const std::string& text) const {
        AssistantMessage message;
        message.api = state.config.model.api;
        message.provider = state.config.model.provider;
        message.model = state.config.model.id;
        message.stopReason = StopReason::Error;
        message.errorMessage = text;
        message.timestamp = m_clock.nowMs();
        return message;
    }

    IToolCallRunner& m_toolCalls;
    IExecutor& m_executor;
    const IClock& m_clock;
    TranscriptNormalizer m_transcript;
};
