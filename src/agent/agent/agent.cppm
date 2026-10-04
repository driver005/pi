export module pi.agent.agent;

import std;
export import pi.agent.i_agent;
export import pi.agent.i_agent_loop;
export import pi.platform.i_clock;
export import pi.support.pending_message_queue;
export import pi.support.transcript_normalizer;
export import pi.types.agent_options;

/** Stateful wrapper around IAgentLoop (port of packages/agent/src/agent.ts). */
export class Agent : public IAgent {
public:
    Agent(AgentOptions options, IAgentLoop& loop, const IClock& clock)
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

    AgentState state() const override {
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

    Model model() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_model;
    }

    ThinkingLevel thinkingLevel() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_thinkingLevel;
    }

    bool isRunning() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_running;
    }

    std::vector<AgentMessage> messages() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages;
    }

    void setModel(const Model& model) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_model = model;
    }

    void setThinkingLevel(ThinkingLevel level) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_thinkingLevel = level;
    }

    void setTools(std::vector<std::shared_ptr<ITool>> tools) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_tools = std::move(tools);
    }

    void setMessages(std::vector<AgentMessage> messages) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_messages = std::move(messages);
    }

    ListenerId subscribe(Listener listener) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const ListenerId id = m_nextListener++;
        m_listeners.emplace(id, std::move(listener));
        return id;
    }

    void unsubscribe(ListenerId id) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(id);
    }

    void setSteeringMode(QueueMode mode) override {
        m_steering.setMode(mode);
    }

    void setFollowUpMode(QueueMode mode) override {
        m_followUps.setMode(mode);
    }

    void steer(AgentMessage message) override {
        m_steering.enqueue(std::move(message));
    }

    void followUp(AgentMessage message) override {
        m_followUps.enqueue(std::move(message));
    }

    void clearSteeringQueue() override {
        m_steering.clear();
    }

    void clearFollowUpQueue() override {
        m_followUps.clear();
    }

    void clearAllQueues() override {
        m_steering.clear();
        m_followUps.clear();
    }

    bool hasQueuedMessages() const override {
        return m_steering.hasItems() || m_followUps.hasItems();
    }

    std::vector<AgentMessage> peekQueuedMessages() const override {
        std::vector<AgentMessage> steering = m_steering.peek();
        return steering.empty() ? m_followUps.peek() : steering;
    }

    Result<void> prompt(std::vector<AgentMessage> messages) override {
        return runWithLifecycle(false, std::move(messages), false);
    }

    Result<void> promptText(const std::string& text, const std::vector<ImageContent>& images) override {
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

    Result<void> continueRun() override {
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

    void abort() override {
        std::shared_ptr<AbortSignal> signal;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            signal = m_signal;
        }
        if (signal != nullptr) {
            signal->abort();
        }
    }

    void waitForIdle() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [this] { return !m_running; });
    }

    Result<void> reset() override {
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

private:
    std::vector<Message> llmMessagesLocked() const {
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

    AgentLoopConfig createLoopConfig(bool skipInitialSteeringPoll) {
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

    Result<void> runWithLifecycle(bool isContinuation, std::vector<AgentMessage> prompts, bool skipInitialSteeringPoll) {
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

    void finishRun() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_streamingMessage.reset();
            m_pendingToolCalls.clear();
            m_signal.reset();
            m_running = false;
        }
        m_idle.notify_all();
    }

    void processEvent(const AgentEvent& event) {
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

    void reduceEvent(const AgentEvent& event) {
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

    Result<void> beginRun(std::shared_ptr<AbortSignal>& signal) {
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

    IAgentLoop& m_loop;
    const IClock& m_clock;
    TranscriptNormalizer m_transcript;
    AgentOptions m_options;
    PendingMessageQueue m_steering;
    PendingMessageQueue m_followUps;

    mutable std::mutex m_mutex;
    std::condition_variable m_idle;
    Model m_model;
    ThinkingLevel m_thinkingLevel;
    std::vector<std::shared_ptr<ITool>> m_tools;
    std::vector<AgentMessage> m_messages;
    bool m_running = false;
    std::shared_ptr<AbortSignal> m_signal;
    std::optional<AgentMessage> m_streamingMessage;
    std::set<std::string> m_pendingToolCalls;
    std::optional<std::string> m_errorMessage;
    std::map<ListenerId, Listener> m_listeners;
    ListenerId m_nextListener = 1;
};
