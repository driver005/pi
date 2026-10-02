module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.session.agent_session;

import std;
export import pi.session.i_agent_session;
export import pi.types.agent_session_config;
import pi.support.agent_message_converter;
import pi.support.auth_guidance;
import pi.support.auto_retry_controller;
import pi.support.branch_navigator;
import pi.support.branch_summarizer;
import pi.support.compaction_controller;
import pi.support.compactor;
import pi.support.context_usage_calculator;
import pi.support.custom_message_queue;
import pi.support.model_controller;
import pi.support.pending_input_tracker;
import pi.support.post_run_handler;
import pi.support.prompt_loadout;
import pi.support.prompt_template_expander;
import pi.support.recovery_attempt_omitter;
import pi.support.session_bash_controller;
import pi.support.session_context_refresher;
import pi.support.session_event_hub;
import pi.support.session_message_persister;
import pi.support.session_stats_calculator;
import pi.support.skill_command_expander;
import pi.support.summary_generator;

/**
 * IAgentSession over an Agent and a session tree: persists every finished message, keeps the
 * agent's context equal to the tree's projection, retries and compacts as needed and exposes the
 * model, tool, bash and tree operations. The behavior of the TypeScript AgentSession without its
 * extension hooks, which plugins will attach later.
 */
export class AgentSession : public IAgentSession {
public:
    explicit AgentSession(AgentSessionConfig config);
    ~AgentSession() override;

    ListenerId subscribe(Listener listener) override;
    void unsubscribe(ListenerId id) override;

    std::vector<AgentMessage> messages() const override;
    Model model() const override;
    ThinkingLevel thinkingLevel() const override;
    std::string systemPrompt() const override;
    std::string sessionId() const override;
    std::optional<std::string> sessionFile() const override;
    std::optional<std::string> sessionName() const override;
    bool isStreaming() const override;
    bool isIdle() const override;
    bool isCompacting() const override;
    bool isRetrying() const override;
    int retryAttempt() const override;
    QueueMode steeringMode() const override;
    QueueMode followUpMode() const override;
    std::size_t pendingMessageCount() const override;
    std::optional<std::string> lastAssistantText() const override;
    SessionStats stats() const override;
    std::optional<ContextUsage> contextUsage() const override;

    Result<PromptDisposition> prompt(const std::string& text, const PromptOptions& options) override;
    Result<PromptDisposition> steer(const std::string& text, const std::vector<ImageContent>& images) override;
    Result<PromptDisposition> followUp(const std::string& text, const std::vector<ImageContent>& images) override;
    Result<void> sendCustomMessage(const std::string& customType, const Json& content, bool display,
                                   const Json& details, const SendMessageOptions& options) override;
    QueuedInput clearQueue() override;
    QueuedInput queued() const override;
    void abort() override;
    void waitForIdle() override;

    Result<void> setModel(const Model& model, bool persist) override;
    std::optional<ModelCycleResult> cycleModel(bool forward, bool persist) override;
    void setThinkingLevel(ThinkingLevel level, bool persist) override;
    std::optional<ThinkingLevel> cycleThinkingLevel(bool persist) override;
    std::vector<ThinkingLevel> availableThinkingLevels() const override;
    bool supportsThinking() const override;
    void setScopedModels(std::vector<ScopedModel> scoped) override;
    void setSteeringMode(QueueMode mode) override;
    void setFollowUpMode(QueueMode mode) override;

    Result<CompactionResult> compact(const std::optional<std::string>& customInstructions) override;
    void abortCompaction() override;
    void abortBranchSummary() override;
    void setAutoCompactionEnabled(bool enabled) override;
    bool autoCompactionEnabled() const override;
    void setAutoRetryEnabled(bool enabled) override;
    bool autoRetryEnabled() const override;
    void abortRetry() override;

    Result<BashResult> executeBash(const std::string& command, const std::function<void(const std::string&)>& onChunk,
                                   bool excludeFromContext, const std::optional<std::string>& id) override;
    void recordBashResult(const std::string& command, const BashResult& result, bool excludeFromContext) override;
    void abortBash() override;
    bool isBashRunning() const override;

    std::vector<std::string> activeToolNames() const override;
    std::vector<ToolInfo> allTools() const override;
    void setActiveToolsByName(const std::vector<std::string>& names) override;

    std::vector<SlashCommandInfo> slashCommands() const override;

    std::vector<SessionEntry> entries() const override;
    std::optional<std::string> leafId() const override;
    std::vector<SessionTreeNode> tree() const override;
    void setSessionName(const std::string& name) override;
    Result<NavigateTreeResult> navigateTree(const std::string& targetId, const NavigateTreeOptions& options) override;
    std::vector<ForkableMessage> forkableMessages() const override;

    Result<void> reload() override;
    void dispose() override;

private:
    // Construction
    void createAgent();
    void createCollaborators();
    StreamOptions streamOptions() const;
    std::optional<AgentLoopTurnUpdate> prepareRequest(const PrepareRequestContext& request) const;
    std::optional<AgentLoopTurnUpdate> prepareNextTurn(const AgentTurnContext& turn);

    // Agent events
    void onAgentEvent(const AgentEvent& event);
    void onMessageStart(const AgentEvent& event);
    void onMessageEnd(const AgentEvent& event);
    void emitAgentEvent(const AgentEvent& event);
    void emitSettled();
    std::string userText(const UserMessage& message) const;

    // Prompt flow
    Result<PromptDisposition> queueWhileStreaming(const std::string& text, const PromptOptions& options);
    Result<void> ensureReady();
    std::vector<AgentMessage> buildPromptMessages(const std::string& text, const std::vector<ImageContent>& images);
    UserMessage userMessage(const std::string& text, const std::vector<ImageContent>& images) const;
    Result<void> runAgentPrompt(std::vector<AgentMessage> messages);
    void continueUntilSettled();
    std::string expand(const std::string& text) const;
    Result<PromptDisposition> queueInput(const std::string& text, const std::vector<ImageContent>& images,
                                         bool steering);
    CustomMessage customMessage(const std::string& customType, const Json& content, bool display,
                                const Json& details) const;
    void flushPending();

    // Helpers
    bool abortRequested() const;
    bool busy() const;
    void notifyIdle();
    QueueMode queueMode(const std::string& name) const;

    AgentSessionConfig m_config;
    AgentMessageConverter m_converter;
    AuthGuidance m_guidance;
    SessionEventHub m_hub;
    SummaryGenerator m_generator;
    Compactor m_compactor;
    BranchSummarizer m_summarizer;
    SkillCommandExpander m_skills;
    PromptTemplateExpander m_templates;
    SessionStatsCalculator m_statsCalculator;
    ContextUsageCalculator m_usageCalculator;

    std::unique_ptr<IAgent> m_agent;
    IAgent::ListenerId m_agentListener = 0;
    std::unique_ptr<SessionContextRefresher> m_refresher;
    std::unique_ptr<RecoveryAttemptOmitter> m_omitter;
    std::unique_ptr<AutoRetryController> m_retry;
    std::unique_ptr<CompactionController> m_compaction;
    std::unique_ptr<BranchNavigator> m_navigator;
    std::unique_ptr<SessionBashController> m_bash;
    std::unique_ptr<ModelController> m_models;
    std::unique_ptr<PromptLoadout> m_loadout;
    std::unique_ptr<PendingInputTracker> m_pending;
    std::unique_ptr<CustomMessageQueue> m_custom;
    std::unique_ptr<SessionMessagePersister> m_persister;
    std::unique_ptr<PostRunHandler> m_postRun;

    std::atomic<bool> m_runActive{false};
    std::atomic<bool> m_abortRequested{false};
    std::mutex m_mutex;
    std::condition_variable m_idle;
    std::optional<AssistantMessage> m_lastAssistant;
    std::vector<ToolResultMessage> m_lastToolResults;
};

AgentSession::AgentSession(AgentSessionConfig config)
    : m_config(std::move(config)),
      m_generator(m_config.clock, m_config.ids, m_config.sleeper),
      m_compactor(m_generator),
      m_summarizer(m_generator),
      m_skills(m_config.files) {
    createAgent();
    createCollaborators();
    m_agentListener = m_agent->subscribe(
        [this](const AgentEvent& event, const std::shared_ptr<AbortSignal>&) { onAgentEvent(event); });
    m_loadout->rebuild();
    if (m_config.initialActiveTools) {
        m_loadout->setActiveTools(*m_config.initialActiveTools);
    } else {
        m_loadout->setActiveTools({"read", "bash", "edit", "write"});
        m_loadout->restoreFromTranscript();
    }
}

AgentSession::~AgentSession() {
    dispose();
}

QueueMode AgentSession::queueMode(const std::string& name) const {
    return name == "all" ? QueueMode::All : QueueMode::OneAtATime;
}

StreamOptions AgentSession::streamOptions() const {
    const SettingsView view = m_config.settings.view();
    StreamOptions options;
    options.sessionId = m_config.session.sessionId();
    options.timeoutMs = view.providerTimeoutMs();
    options.maxRetries = view.providerMaxRetries() ? std::optional<int>(static_cast<int>(*view.providerMaxRetries()))
                                                   : std::nullopt;
    options.maxRetryDelayMs = view.providerMaxRetryDelayMs();
    options.websocketConnectTimeoutMs = view.websocketConnectTimeoutMs();
    options.transport = view.transport();
    options.thinkingBudgets = view.thinkingBudgets();
    return options;
}

void AgentSession::createAgent() {
    const SettingsView view = m_config.settings.view();
    AgentOptions options;
    options.model = m_config.model;
    options.thinkingLevel = m_config.thinkingLevel;
    options.messages = m_config.session.buildSessionContext().messages;
    options.steeringMode = queueMode(view.steeringMode());
    options.followUpMode = queueMode(view.followUpMode());
    options.streamFn = [this](const Model& model, const TranscriptContext& context, const StreamOptions& request) {
        return m_config.models.stream(model, context, request);
    };
    options.loopConfig.options = streamOptions();
    options.loopConfig.convertToLlm = [this](const std::vector<AgentMessage>& messages) {
        return m_converter.convert(messages);
    };
    options.loopConfig.prepareRequest = [this](const PrepareRequestContext& request,
                                               const std::shared_ptr<AbortSignal>&) { return prepareRequest(request); };
    options.loopConfig.prepareNextTurn = [this](const AgentTurnContext& turn) { return prepareNextTurn(turn); };
    m_agent = m_config.agents.create(std::move(options));
}

void AgentSession::createCollaborators() {
    ISessionManager& session = m_config.session;
    ISettingsManager& settings = m_config.settings;
    const StreamFn streamFn = [this](const Model& model, const TranscriptContext& context, const StreamOptions& request) {
        return m_config.models.stream(model, context, request);
    };
    m_refresher = std::make_unique<SessionContextRefresher>(*m_agent, session);
    m_omitter = std::make_unique<RecoveryAttemptOmitter>(session, m_hub, *m_refresher);
    m_retry = std::make_unique<AutoRetryController>(settings, m_config.sleeper, *m_omitter, m_hub);
    m_compaction = std::make_unique<CompactionController>(*m_agent, session, settings, *m_refresher, m_hub,
                                                          m_compactor, streamFn);
    m_navigator = std::make_unique<BranchNavigator>(*m_agent, session, settings, *m_refresher, m_hub, m_summarizer,
                                                    streamFn);
    m_bash = std::make_unique<SessionBashController>(*m_agent, session, settings, *m_refresher, m_hub,
                                                     m_config.bash, m_config.clock);
    m_models = std::make_unique<ModelController>(*m_agent, session, settings, m_config.models, m_hub);
    m_models->setScopedModels(m_config.scopedModels);
    m_loadout = std::make_unique<PromptLoadout>(*m_agent, session, m_config.tools, m_config.resources,
                                                m_config.clock, m_config.cwd);
    m_loadout->setToolFilter(m_config.allowedTools, m_config.excludedTools);
    m_pending = std::make_unique<PendingInputTracker>(m_hub);
    m_custom = std::make_unique<CustomMessageQueue>(session, *m_refresher, m_hub);
    m_persister = std::make_unique<SessionMessagePersister>(session);
    m_postRun = std::make_unique<PostRunHandler>(*m_agent, session, settings, *m_retry, *m_compaction, *m_omitter, m_hub);
}

std::optional<AgentLoopTurnUpdate> AgentSession::prepareRequest(const PrepareRequestContext& request) const {
    AgentLoopTurnUpdate update;
    AgentContext context;
    context.messages = m_config.session.buildSessionProjection().messages;
    if (request.context != nullptr) {
        context.tools = request.context->tools;
    }
    update.context = std::move(context);
    return update;
}

std::optional<AgentLoopTurnUpdate> AgentSession::prepareNextTurn(const AgentTurnContext&) {
    std::vector<AgentMessage> messages = m_postRun->contextForNextResponse();
    BuildSystemPromptOptions options = m_loadout->baseOptions();
    options.selectedTools = m_loadout->activeToolNames();
    const std::optional<SystemMessage> update = m_loadout->prepare(options, messages);
    AgentLoopTurnUpdate result;
    AgentContext context;
    context.messages = std::move(messages);
    context.tools = m_config.tools.active();
    result.context = std::move(context);
    if (update) {
        result.messages.emplace_back(*update);
    }
    result.model = m_agent->model();
    result.thinkingLevel = m_agent->thinkingLevel();
    return result;
}

// ---------------------------------------------------------------------------
// Agent events
// ---------------------------------------------------------------------------

std::string AgentSession::userText(const UserMessage& message) const {
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        return *text;
    }
    std::string out;
    for (const auto& block : std::get<std::vector<UserContentBlock>>(message.content)) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            out += text->text;
        }
    }
    return out;
}

void AgentSession::emitAgentEvent(const AgentEvent& event) {
    AgentSessionEvent out;
    out.type = event.type == AgentEventType::AgentEnd ? SessionEventType::AgentEnd : SessionEventType::Agent;
    out.agent = std::make_shared<const AgentEvent>(event);
    if (event.type == AgentEventType::AgentEnd) {
        out.willRetry = m_retry->willRetryAfterAgentEnd(event.messages, m_agent->model().contextWindow,
                                                        abortRequested());
    }
    m_hub.emit(out);
}

void AgentSession::onMessageStart(const AgentEvent& event) {
    if (event.message == nullptr) {
        return;
    }
    if (const auto* user = std::get_if<UserMessage>(event.message.get())) {
        m_postRun->resetOverflowRecovery();
        m_pending->delivered(userText(*user));
    }
}

void AgentSession::onMessageEnd(const AgentEvent& event) {
    if (event.message == nullptr) {
        return;
    }
    m_persister->persist(*event.message);
    const auto* assistant = std::get_if<AssistantMessage>(event.message.get());
    if (assistant == nullptr) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_lastAssistant = *assistant;
    }
    if (assistant->stopReason != StopReason::Error && assistant->stopReason != StopReason::Length) {
        m_postRun->resetOverflowRecovery();
    }
    if (assistant->stopReason != StopReason::Error) {
        m_retry->succeeded();
    }
}

void AgentSession::onAgentEvent(const AgentEvent& event) {
    if (event.type == AgentEventType::MessageStart) {
        onMessageStart(event);
    }
    emitAgentEvent(event);
    if (event.type == AgentEventType::MessageEnd) {
        onMessageEnd(event);
    } else if (event.type == AgentEventType::TurnEnd) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_lastToolResults = event.toolResults;
        }
        m_custom->flush();
    }
}

// ---------------------------------------------------------------------------
// Idle tracking
// ---------------------------------------------------------------------------

bool AgentSession::abortRequested() const {
    return m_abortRequested.load();
}

bool AgentSession::isStreaming() const {
    return m_runActive.load();
}

bool AgentSession::isCompacting() const {
    return m_compaction->compacting() || m_navigator->summarizing();
}

bool AgentSession::isIdle() const {
    return !isStreaming() && !isCompacting();
}

bool AgentSession::busy() const {
    return !isIdle();
}

void AgentSession::notifyIdle() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
    }
    m_idle.notify_all();
}

void AgentSession::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (busy()) {
        m_idle.wait_for(lock, std::chrono::milliseconds(20));
    }
}

void AgentSession::emitSettled() {
    m_runActive = false;
    AgentSessionEvent event;
    event.type = SessionEventType::AgentSettled;
    m_hub.emit(event);
    notifyIdle();
}

// ---------------------------------------------------------------------------
// Prompting
// ---------------------------------------------------------------------------

std::string AgentSession::expand(const std::string& text) const {
    const auto skill = m_skills.expand(text, m_config.resources.resources().skills);
    const std::string expanded = skill ? *skill : text;
    return m_templates.expand(expanded, m_config.resources.resources().promptTemplates);
}

UserMessage AgentSession::userMessage(const std::string& text, const std::vector<ImageContent>& images) const {
    UserMessage message;
    std::vector<UserContentBlock> blocks{TextContent{text, std::nullopt}};
    for (const auto& image : images) {
        blocks.emplace_back(image);
    }
    message.content = std::move(blocks);
    message.timestamp = m_config.clock.nowMs();
    return message;
}

Result<PromptDisposition> AgentSession::queueInput(const std::string& text, const std::vector<ImageContent>& images,
                                                   bool steering) {
    const std::string expanded = expand(text);
    if (steering) {
        m_pending->queueSteering(expanded);
        m_agent->steer(userMessage(expanded, images));
    } else {
        m_pending->queueFollowUp(expanded);
        m_agent->followUp(userMessage(expanded, images));
    }
    return PromptDisposition::Queued;
}

Result<PromptDisposition> AgentSession::steer(const std::string& text, const std::vector<ImageContent>& images) {
    return queueInput(text, images, true);
}

Result<PromptDisposition> AgentSession::followUp(const std::string& text, const std::vector<ImageContent>& images) {
    return queueInput(text, images, false);
}

Result<PromptDisposition> AgentSession::queueWhileStreaming(const std::string& text, const PromptOptions& options) {
    if (options.streamingBehavior == StreamingBehavior::None) {
        return std::unexpected(Error{"agent_busy",
                                     "Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message."});
    }
    return queueInput(text, options.images, options.streamingBehavior == StreamingBehavior::Steer);
}

Result<void> AgentSession::ensureReady() {
    const Model model = m_agent->model();
    if (model.id.empty()) {
        return std::unexpected(Error{"no_model", m_guidance.noModelSelected()});
    }
    if (!m_config.models.hasConfiguredAuth(model.provider)) {
        return std::unexpected(Error{"no_auth", m_guidance.noApiKeyFound(model.provider)});
    }
    return {};
}

void AgentSession::flushPending() {
    m_bash->flushPending();
    m_custom->flush();
}

std::vector<AgentMessage> AgentSession::buildPromptMessages(const std::string& text,
                                                            const std::vector<ImageContent>& images) {
    std::vector<AgentMessage> messages;
    messages.emplace_back(userMessage(text, images));
    for (auto& queued : m_custom->takeNextTurn()) {
        messages.emplace_back(std::move(queued));
    }
    BuildSystemPromptOptions options = m_loadout->baseOptions();
    options.selectedTools = m_loadout->activeToolNames();
    if (const auto update = m_loadout->prepare(options, m_agent->messages())) {
        messages.insert(messages.begin(), AgentMessage(*update));
    }
    return messages;
}

void AgentSession::continueUntilSettled() {
    while (!abortRequested()) {
        std::optional<AssistantMessage> last;
        std::vector<ToolResultMessage> toolResults;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            last = std::exchange(m_lastAssistant, std::nullopt);
            toolResults = std::exchange(m_lastToolResults, {});
        }
        if (!m_postRun->afterRun(last, toolResults, [this] { return abortRequested(); }) || abortRequested()) {
            break;
        }
        if (const auto resumed = m_agent->continueRun(); !resumed) {
            break;
        }
    }
}

Result<void> AgentSession::runAgentPrompt(std::vector<AgentMessage> messages) {
    m_abortRequested = false;
    m_runActive = true;
    Result<void> outcome = m_agent->prompt(std::move(messages));
    if (outcome) {
        continueUntilSettled();
    }
    if (abortRequested()) {
        m_retry->finishCancelled();
    }
    m_bash->flushPending();
    m_custom->flush();
    emitSettled();
    return outcome;
}

Result<PromptDisposition> AgentSession::prompt(const std::string& text, const PromptOptions& options) {
    if (m_compaction->manualInProgress()) {
        return std::unexpected(Error{"compacting",
                                     "Cannot submit a prompt while compaction is in progress. Wait for compaction to finish and retry."});
    }
    const std::string expanded = options.expandPromptTemplates ? expand(text) : text;
    if (isStreaming()) {
        auto queued = queueWhileStreaming(expanded, options);
        if (queued && options.onDisposition) {
            options.onDisposition(PromptDisposition::Queued);
        }
        return queued;
    }
    flushPending();
    if (auto ready = ensureReady(); !ready) {
        return std::unexpected(ready.error());
    }
    // A response that ended in abort or error may already call for compaction; the new prompt
    // is sent right after, so nothing is continued here.
    const std::vector<AgentMessage> existing = m_agent->messages();
    for (auto it = existing.rbegin(); it != existing.rend(); ++it) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&*it)) {
            m_postRun->checkCompaction(*assistant, false, {});
            break;
        }
    }
    std::vector<AgentMessage> messages = buildPromptMessages(expanded, options.images);
    if (options.onDisposition) {
        options.onDisposition(PromptDisposition::Started);
    }
    if (auto started = runAgentPrompt(std::move(messages)); !started) {
        return std::unexpected(started.error());
    }
    return PromptDisposition::Started;
}

CustomMessage AgentSession::customMessage(const std::string& customType, const Json& content, bool display,
                                          const Json& details) const {
    CustomMessage message;
    message.role = "custom";
    message.timestamp = m_config.clock.nowMs();
    message.data = Json::object();
    message.data["customType"] = customType;
    message.data["content"] = content.is_null() ? Json::array() : content;
    message.data["display"] = display;
    if (!details.is_null()) {
        message.data["details"] = details;
    }
    return message;
}

Result<void> AgentSession::sendCustomMessage(const std::string& customType, const Json& content, bool display,
                                             const Json& details, const SendMessageOptions& options) {
    const CustomMessage message = customMessage(customType, content, display, details);
    if (options.deliverAs == DeliverAs::NextTurn) {
        m_custom->queueForNextTurn(message);
    } else if (isStreaming() && options.triggerTurn != false) {
        if (options.deliverAs == DeliverAs::FollowUp) {
            m_agent->followUp(message);
        } else {
            m_agent->steer(message);
        }
    } else if (options.triggerTurn == true) {
        return runAgentPrompt({AgentMessage(message)});
    } else if (isStreaming()) {
        m_custom->defer(message);
    } else {
        m_custom->appendNow(message);
    }
    return {};
}

QueuedInput AgentSession::clearQueue() {
    m_agent->clearAllQueues();
    return m_pending->clear();
}

QueuedInput AgentSession::queued() const {
    return m_pending->snapshot();
}

void AgentSession::abort() {
    if (m_runActive) {
        m_abortRequested = true;
    }
    m_retry->abort();
    m_compaction->abort();
    m_navigator->abort();
    m_agent->abort();
}


// ---------------------------------------------------------------------------
// Subscription and state
// ---------------------------------------------------------------------------

IAgentSession::ListenerId AgentSession::subscribe(Listener listener) {
    return m_hub.subscribe(std::move(listener));
}

void AgentSession::unsubscribe(ListenerId id) {
    m_hub.unsubscribe(id);
}

std::vector<AgentMessage> AgentSession::messages() const {
    return m_agent->messages();
}

Model AgentSession::model() const {
    return m_agent->model();
}

ThinkingLevel AgentSession::thinkingLevel() const {
    return m_agent->thinkingLevel();
}

std::string AgentSession::systemPrompt() const {
    return m_loadout->systemPromptText();
}

std::string AgentSession::sessionId() const {
    return m_config.session.sessionId();
}

std::optional<std::string> AgentSession::sessionFile() const {
    return m_config.session.sessionFile();
}

std::optional<std::string> AgentSession::sessionName() const {
    return m_config.session.sessionName();
}

bool AgentSession::isRetrying() const {
    return m_retry->retrying();
}

int AgentSession::retryAttempt() const {
    return m_retry->attempt();
}

QueueMode AgentSession::steeringMode() const {
    return queueMode(m_config.settings.view().steeringMode());
}

QueueMode AgentSession::followUpMode() const {
    return queueMode(m_config.settings.view().followUpMode());
}

std::size_t AgentSession::pendingMessageCount() const {
    return m_pending->count();
}

std::optional<std::string> AgentSession::lastAssistantText() const {
    const std::vector<AgentMessage> all = m_agent->messages();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const auto* assistant = std::get_if<AssistantMessage>(&*it);
        if (assistant == nullptr || (assistant->stopReason == StopReason::Aborted && assistant->content.empty())) {
            continue;
        }
        std::string text;
        for (const auto& block : assistant->content) {
            if (const auto* part = std::get_if<TextContent>(&block)) {
                text += part->text;
            }
        }
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::nullopt;
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }
    return std::nullopt;
}

std::optional<ContextUsage> AgentSession::contextUsage() const {
    const Model current = m_agent->model();
    if (current.id.empty()) {
        return std::nullopt;
    }
    return m_usageCalculator.calculate(current.contextWindow, m_config.session.buildSessionProjection(),
                                       m_config.session.branchPath());
}

SessionStats AgentSession::stats() const {
    SessionStats result = m_statsCalculator.calculate(m_config.session.entries());
    result.sessionFile = m_config.session.sessionFile();
    result.sessionId = m_config.session.sessionId();
    result.contextUsage = contextUsage();
    return result;
}

// ---------------------------------------------------------------------------
// Model, thinking and queue modes
// ---------------------------------------------------------------------------

Result<void> AgentSession::setModel(const Model& model, bool persist) {
    return m_models->setModel(model, persist);
}

std::optional<ModelCycleResult> AgentSession::cycleModel(bool forward, bool persist) {
    return m_models->cycleModel(forward, persist);
}

void AgentSession::setThinkingLevel(ThinkingLevel level, bool persist) {
    m_models->setThinkingLevel(level, persist);
}

std::optional<ThinkingLevel> AgentSession::cycleThinkingLevel(bool persist) {
    return m_models->cycleThinkingLevel(persist);
}

std::vector<ThinkingLevel> AgentSession::availableThinkingLevels() const {
    return m_models->availableThinkingLevels();
}

bool AgentSession::supportsThinking() const {
    return m_models->supportsThinking();
}

void AgentSession::setScopedModels(std::vector<ScopedModel> scoped) {
    m_models->setScopedModels(std::move(scoped));
}

void AgentSession::setSteeringMode(QueueMode mode) {
    m_agent->setSteeringMode(mode);
    m_config.settings.setGlobal("steeringMode", mode == QueueMode::All ? "all" : "one-at-a-time");
}

void AgentSession::setFollowUpMode(QueueMode mode) {
    m_agent->setFollowUpMode(mode);
    m_config.settings.setGlobal("followUpMode", mode == QueueMode::All ? "all" : "one-at-a-time");
}

// ---------------------------------------------------------------------------
// Compaction and retry
// ---------------------------------------------------------------------------

Result<CompactionResult> AgentSession::compact(const std::optional<std::string>& customInstructions) {
    abort();
    waitForIdle();
    auto result = m_compaction->compactManual(customInstructions);
    notifyIdle();
    return result;
}

void AgentSession::abortCompaction() {
    m_compaction->abort();
}

void AgentSession::abortBranchSummary() {
    m_navigator->abort();
}

void AgentSession::setAutoCompactionEnabled(bool enabled) {
    m_config.settings.setGlobalNested("compaction", "enabled", enabled);
}

bool AgentSession::autoCompactionEnabled() const {
    return m_config.settings.view().compactionEnabled();
}

void AgentSession::setAutoRetryEnabled(bool enabled) {
    m_config.settings.setGlobalNested("retry", "enabled", enabled);
}

bool AgentSession::autoRetryEnabled() const {
    return m_config.settings.view().retryEnabled();
}

void AgentSession::abortRetry() {
    m_retry->abort();
}

// ---------------------------------------------------------------------------
// Bash, tools and the session tree
// ---------------------------------------------------------------------------

Result<BashResult> AgentSession::executeBash(const std::string& command,
                                             const std::function<void(const std::string&)>& onChunk,
                                             bool excludeFromContext, const std::optional<std::string>& id) {
    return m_bash->execute(command, onChunk, excludeFromContext, id);
}

void AgentSession::recordBashResult(const std::string& command, const BashResult& result, bool excludeFromContext) {
    m_bash->record(command, result, excludeFromContext);
}

void AgentSession::abortBash() {
    m_bash->abort();
}

bool AgentSession::isBashRunning() const {
    return m_bash->running();
}

std::vector<std::string> AgentSession::activeToolNames() const {
    return m_loadout->activeToolNames();
}

std::vector<ToolInfo> AgentSession::allTools() const {
    const std::vector<std::string> active = m_loadout->activeToolNames();
    std::vector<ToolInfo> out;
    for (const auto& tool : m_config.tools.all()) {
        const Tool& definition = tool->definition();
        ToolInfo info;
        info.name = definition.name;
        info.description = definition.description;
        info.parameters = definition.parameters;
        info.promptSnippet = tool->promptSnippet();
        info.promptGuidelines = tool->promptGuidelines();
        info.active = std::ranges::find(active, definition.name) != active.end();
        out.push_back(std::move(info));
    }
    return out;
}

void AgentSession::setActiveToolsByName(const std::vector<std::string>& names) {
    m_loadout->setActiveTools(names);
}

void AgentSession::setSessionName(const std::string& name) {
    m_config.session.appendSessionInfo(name);
    AgentSessionEvent event;
    event.type = SessionEventType::SessionInfoChanged;
    event.name = m_config.session.sessionName();
    m_hub.emit(event);
}

Result<NavigateTreeResult> AgentSession::navigateTree(const std::string& targetId, const NavigateTreeOptions& options) {
    if (isStreaming()) {
        return std::unexpected(Error{"busy", "Wait for the current response to finish before navigating the session tree."});
    }
    if (isCompacting()) {
        return std::unexpected(Error{"busy", "Wait for the current compaction or tree navigation to finish before navigating the session tree."});
    }
    auto result = m_navigator->navigate(targetId, options);
    if (result) {
        m_loadout->restoreFromTranscript();
    }
    notifyIdle();
    return result;
}

std::vector<SlashCommandInfo> AgentSession::slashCommands() const {
    const LoadedResources resources = m_config.resources.resources();
    std::vector<SlashCommandInfo> out;
    for (const auto& prompt : resources.promptTemplates) {
        out.push_back(SlashCommandInfo{prompt.name, prompt.description, "prompt", prompt.sourceInfo});
    }
    for (const auto& skill : resources.skills) {
        out.push_back(SlashCommandInfo{"skill:" + skill.name, skill.description, "skill", skill.sourceInfo});
    }
    return out;
}

std::vector<SessionEntry> AgentSession::entries() const {
    return m_config.session.entries();
}

std::optional<std::string> AgentSession::leafId() const {
    return m_config.session.leafId();
}

std::vector<SessionTreeNode> AgentSession::tree() const {
    return m_config.session.tree();
}

std::vector<ForkableMessage> AgentSession::forkableMessages() const {
    return m_navigator->forkableMessages();
}

Result<void> AgentSession::reload() {
    m_config.settings.reload();
    m_agent->setSteeringMode(queueMode(m_config.settings.view().steeringMode()));
    m_agent->setFollowUpMode(queueMode(m_config.settings.view().followUpMode()));
    if (auto loaded = m_config.resources.reload(); !loaded) {
        return loaded;
    }
    m_loadout->rebuild();
    return {};
}

void AgentSession::dispose() {
    if (m_agent == nullptr) {
        return;
    }
    abort();
    m_bash->abort();
    m_agent->unsubscribe(m_agentListener);
    m_hub.clear();
}
