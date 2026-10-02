#include "src/agent/agent/agent.h"

Agent::Agent(AgentOptions options, IAgentLoop& loop, const IClock& clock)
    : m_loop(loop),
      m_clock(clock),
      m_options(std::move(options)),
      m_steering(m_options.steeringMode),
      m_followUps(m_options.followUpMode),
      m_model(m_options.model),
      m_thinkingLevel(m_options.thinkingLevel),
      m_tools(m_options.tools),
      m_messages(m_options.messages) {
    std::vector<Tool> declarations;
    for (const auto& tool : m_tools) {
        declarations.push_back(m_transcript.toToolDeclaration(tool->definition()));
    }
    const bool startsWithSystem =
        !m_messages.empty() && std::holds_alternative<SystemMessage>(m_messages.front());
    if (auto initial = m_transcript.createInitialSystemMessage(m_options.systemPrompt, declarations);
        initial.has_value() && !startsWithSystem) {
        m_messages.insert(m_messages.begin(), AgentMessage(std::move(*initial)));
    }
}

std::vector<Message> Agent::llmMessagesLocked() const {
    std::vector<Message> out;
    for (const AgentMessage& message : m_messages) {
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

AgentState Agent::state() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    AgentState snapshot;
    snapshot.systemPrompt = m_transcript.currentSystemPrompt(llmMessagesLocked());
    snapshot.model = m_model;
    snapshot.thinkingLevel = m_thinkingLevel;
    snapshot.tools = m_tools;
    snapshot.messages = m_messages;
    snapshot.isStreaming = m_running;
    snapshot.streamingMessage = m_streamingMessage;
    snapshot.pendingToolCalls = m_pendingToolCalls;
    snapshot.errorMessage = m_errorMessage;
    return snapshot;
}

void Agent::setModel(const Model& model) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_model = model;
}

void Agent::setThinkingLevel(ThinkingLevel level) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_thinkingLevel = level;
}

void Agent::setTools(std::vector<std::shared_ptr<ITool>> tools) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_tools = std::move(tools);
}

void Agent::setMessages(std::vector<AgentMessage> messages) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_messages = std::move(messages);
}

IAgent::ListenerId Agent::subscribe(Listener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const ListenerId id = m_nextListener++;
    m_listeners.emplace(id, std::move(listener));
    return id;
}

void Agent::unsubscribe(ListenerId id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.erase(id);
}

void Agent::setSteeringMode(QueueMode mode) {
    m_steering.setMode(mode);
}

void Agent::setFollowUpMode(QueueMode mode) {
    m_followUps.setMode(mode);
}

void Agent::steer(AgentMessage message) {
    m_steering.enqueue(std::move(message));
}

void Agent::followUp(AgentMessage message) {
    m_followUps.enqueue(std::move(message));
}

void Agent::clearSteeringQueue() {
    m_steering.clear();
}

void Agent::clearFollowUpQueue() {
    m_followUps.clear();
}

void Agent::clearAllQueues() {
    m_steering.clear();
    m_followUps.clear();
}

bool Agent::hasQueuedMessages() const {
    return m_steering.hasItems() || m_followUps.hasItems();
}

std::vector<AgentMessage> Agent::peekQueuedMessages() const {
    std::vector<AgentMessage> steering = m_steering.peek();
    return steering.empty() ? m_followUps.peek() : steering;
}

void Agent::abort() {
    std::shared_ptr<AbortSignal> signal;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        signal = m_signal;
    }
    if (signal != nullptr) {
        signal->abort();
    }
}

void Agent::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return !m_running; });
}

Result<void> Agent::reset() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        return std::unexpected(
            Error{"agent_busy", "Agent is already processing. Wait for completion before resetting."});
    }
    auto baseline = m_transcript.currentSystemMessage(llmMessagesLocked());
    m_messages.clear();
    if (baseline.has_value()) {
        m_messages.emplace_back(std::move(*baseline));
    }
    m_streamingMessage.reset();
    m_pendingToolCalls.clear();
    m_errorMessage.reset();
    m_steering.clear();
    m_followUps.clear();
    return {};
}

AgentLoopConfig Agent::createLoopConfig(bool skipInitialSteeringPoll) {
    AgentLoopConfig config = m_options.loopConfig;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        config.model = m_model;
        config.options.reasoning = m_thinkingLevel;
    }
    auto skip = std::make_shared<bool>(skipInitialSteeringPoll);
    config.getSteeringMessages = [this, skip]() -> std::vector<AgentMessage> {
        if (*skip) {
            *skip = false;
            return {};
        }
        return m_steering.drain();
    };
    config.getFollowUpMessages = [this]() { return m_followUps.drain(); };
    return config;
}

Result<void> Agent::beginRun(std::shared_ptr<AbortSignal>& signal) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        return std::unexpected(Error{
            "agent_busy",
            "Agent is already processing a prompt. Use steer() or followUp() to queue messages, or wait "
            "for completion."});
    }
    m_running = true;
    signal = std::make_shared<AbortSignal>();
    m_signal = signal;
    m_streamingMessage.reset();
    m_errorMessage.reset();
    return {};
}

void Agent::finishRun() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_streamingMessage.reset();
        m_pendingToolCalls.clear();
        m_signal.reset();
        m_running = false;
    }
    m_idle.notify_all();
}

void Agent::reduceEvent(const AgentEvent& event) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    switch (event.type) {
        case AgentEventType::MessageStart:
        case AgentEventType::MessageUpdate:
            m_streamingMessage = *event.message;
            break;
        case AgentEventType::MessageEnd:
            m_streamingMessage.reset();
            m_messages.push_back(*event.message);
            break;
        case AgentEventType::ToolExecutionStart:
            m_pendingToolCalls.insert(event.toolCallId);
            break;
        case AgentEventType::ToolExecutionEnd:
            m_pendingToolCalls.erase(event.toolCallId);
            break;
        case AgentEventType::TurnEnd:
            if (const auto* assistant = std::get_if<AssistantMessage>(event.message.get());
                assistant != nullptr && assistant->errorMessage.has_value()) {
                m_errorMessage = assistant->errorMessage;
            }
            break;
        case AgentEventType::AgentEnd:
            m_streamingMessage.reset();
            break;
        default:
            break;
    }
}

void Agent::processEvent(const AgentEvent& event) {
    reduceEvent(event);
    std::vector<Listener> listeners;
    std::shared_ptr<AbortSignal> signal;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_listeners) {
            listeners.push_back(entry.second);
        }
        signal = m_signal;
    }
    for (const Listener& listener : listeners) {
        listener(event, signal);
    }
}

Result<void> Agent::runWithLifecycle(bool isContinuation, std::vector<AgentMessage> prompts,
                                     bool skipInitialSteeringPoll) {
    std::shared_ptr<AbortSignal> signal;
    if (auto begun = beginRun(signal); !begun.has_value()) {
        return begun;
    }
    AgentContext context;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        context.messages = m_messages;
        context.tools = m_tools;
    }
    const AgentLoopConfig config = createLoopConfig(skipInitialSteeringPoll);
    const AgentEventSink sink = [this](const AgentEvent& event) { processEvent(event); };
    Result<void> outcome;
    if (isContinuation) {
        auto result = m_loop.runContinue(std::move(context), config, sink, signal, m_options.streamFn);
        if (!result.has_value()) {
            outcome = std::unexpected(result.error());
        }
    } else {
        m_loop.run(std::move(prompts), std::move(context), config, sink, signal, m_options.streamFn);
    }
    finishRun();
    return outcome;
}

Result<void> Agent::prompt(std::vector<AgentMessage> messages) {
    return runWithLifecycle(false, std::move(messages), false);
}

Result<void> Agent::promptText(const std::string& text, const std::vector<ImageContent>& images) {
    UserMessage message;
    std::vector<UserContentBlock> blocks;
    blocks.emplace_back(TextContent{text, std::nullopt});
    for (const ImageContent& image : images) {
        blocks.emplace_back(image);
    }
    message.content = std::move(blocks);
    message.timestamp = m_clock.nowMs();
    return prompt({AgentMessage(std::move(message))});
}

Result<void> Agent::continueRun() {
    bool hasNonSystem = false;
    bool lastIsAssistant = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return std::unexpected(
                Error{"agent_busy", "Agent is already processing. Wait for completion before continuing."});
        }
        for (const AgentMessage& message : m_messages) {
            hasNonSystem = hasNonSystem || !std::holds_alternative<SystemMessage>(message);
        }
        lastIsAssistant = !m_messages.empty() && std::holds_alternative<AssistantMessage>(m_messages.back());
    }
    if (!hasNonSystem) {
        return std::unexpected(Error{"nothing_to_continue", "No messages to continue from"});
    }
    if (!lastIsAssistant) {
        return runWithLifecycle(true, {}, false);
    }
    if (auto steering = m_steering.drain(); !steering.empty()) {
        return runWithLifecycle(false, std::move(steering), true);
    }
    if (auto followUps = m_followUps.drain(); !followUps.empty()) {
        return runWithLifecycle(false, std::move(followUps), false);
    }
    return std::unexpected(Error{"nothing_to_continue", "Cannot continue from message role: assistant"});
}
