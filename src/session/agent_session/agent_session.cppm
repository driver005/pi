module;

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
import pi.support.plugin_hook_dispatcher;
import pi.support.post_run_handler;
import pi.support.prompt_loadout;
import pi.support.prompt_template_expander;
import pi.support.recovery_attempt_omitter;
import pi.support.session_bash_controller;
import pi.support.session_context_refresher;
import pi.support.session_event_codec;
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
    explicit AgentSession(AgentSessionConfig config)
        : m_config(std::move(config)),
          m_generator(m_config.clock, m_config.ids, m_config.sleeper),
          m_compactor(m_generator),
          m_summarizer(m_generator),
          m_skills(m_config.files) {
        if (m_config.hooks != nullptr) {
            m_hooks = std::make_unique<PluginHookDispatcher>(*m_config.hooks);
        }
        createAgent();
        createCollaborators();
        m_agentListener = m_agent->subscribe(
            [this](const AgentEvent& event, const std::shared_ptr<AbortSignal>&) { onAgentEvent(event); });
        if (m_hooks) {
            m_hub.subscribe([this](const AgentSessionEvent& event) { m_hooks->notify(m_eventCodec.toJson(event)); });
        }
        m_loadout->rebuild();
        if (m_config.initialActiveTools) {
            m_loadout->setActiveTools(*m_config.initialActiveTools);
        } else {
            m_loadout->setActiveTools({"read", "bash", "edit", "write"});
            m_loadout->restoreFromTranscript();
        }
    }

    ~AgentSession() override {
        dispose();
    }

    ListenerId subscribe(Listener listener) override {
        return m_hub.subscribe(std::move(listener));
    }

    void unsubscribe(ListenerId id) override {
        m_hub.unsubscribe(id);
    }

    std::vector<AgentMessage> messages() const override {
        return m_agent->messages();
    }

    Model model() const override {
        return m_agent->model();
    }

    ThinkingLevel thinkingLevel() const override {
        return m_agent->thinkingLevel();
    }

    std::string systemPrompt() const override {
        return m_loadout->systemPromptText();
    }

    std::string sessionId() const override {
        return m_config.session.sessionId();
    }

    std::optional<std::string> sessionFile() const override {
        return m_config.session.sessionFile();
    }

    std::optional<std::string> sessionName() const override {
        return m_config.session.sessionName();
    }

    bool isStreaming() const override {
        return m_runActive.load();
    }

    bool isIdle() const override {
        return !isStreaming() && !isCompacting();
    }

    bool isCompacting() const override {
        return m_compaction->compacting() || m_navigator->summarizing();
    }

    bool isRetrying() const override {
        return m_retry->retrying();
    }

    int retryAttempt() const override {
        return m_retry->attempt();
    }

    QueueMode steeringMode() const override {
        return queueMode(m_config.settings.view().steeringMode());
    }

    QueueMode followUpMode() const override {
        return queueMode(m_config.settings.view().followUpMode());
    }

    std::size_t pendingMessageCount() const override {
        return m_pending->count();
    }

    std::optional<std::string> lastAssistantText() const override {
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

    SessionStats stats() const override {
        SessionStats result = m_statsCalculator.calculate(m_config.session.entries());
        result.sessionFile = m_config.session.sessionFile();
        result.sessionId = m_config.session.sessionId();
        result.contextUsage = contextUsage();
        return result;
    }

    std::optional<ContextUsage> contextUsage() const override {
        const Model current = m_agent->model();
        if (current.id.empty()) {
            return std::nullopt;
        }
        return m_usageCalculator.calculate(current.contextWindow, m_config.session.buildSessionProjection(),
                                           m_config.session.branchPath());
    }

    Result<PromptDisposition> prompt(const std::string& text, const PromptOptions& options) override {
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

    Result<PromptDisposition> steer(const std::string& text, const std::vector<ImageContent>& images) override {
        return queueInput(text, images, true);
    }

    Result<PromptDisposition> followUp(const std::string& text, const std::vector<ImageContent>& images) override {
        return queueInput(text, images, false);
    }

    Result<void> sendCustomMessage(const std::string& customType, const Json& content, bool display, const Json& details, const SendMessageOptions& options) override {
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

    QueuedInput clearQueue() override {
        m_agent->clearAllQueues();
        return m_pending->clear();
    }

    QueuedInput queued() const override {
        return m_pending->snapshot();
    }

    void abort() override {
        if (m_runActive) {
            m_abortRequested = true;
        }
        m_retry->abort();
        m_compaction->abort();
        m_navigator->abort();
        m_agent->abort();
    }

    void waitForIdle() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (busy()) {
            m_idle.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    Result<void> setModel(const Model& model, bool persist) override {
        return m_models->setModel(model, persist);
    }

    std::optional<ModelCycleResult> cycleModel(bool forward, bool persist) override {
        return m_models->cycleModel(forward, persist);
    }

    void setThinkingLevel(ThinkingLevel level, bool persist) override {
        m_models->setThinkingLevel(level, persist);
    }

    std::optional<ThinkingLevel> cycleThinkingLevel(bool persist) override {
        return m_models->cycleThinkingLevel(persist);
    }

    std::vector<ThinkingLevel> availableThinkingLevels() const override {
        return m_models->availableThinkingLevels();
    }

    bool supportsThinking() const override {
        return m_models->supportsThinking();
    }

    void setScopedModels(std::vector<ScopedModel> scoped) override {
        m_models->setScopedModels(std::move(scoped));
    }

    void setSteeringMode(QueueMode mode) override {
        m_agent->setSteeringMode(mode);
        m_config.settings.setGlobal("steeringMode", mode == QueueMode::All ? "all" : "one-at-a-time");
    }

    void setFollowUpMode(QueueMode mode) override {
        m_agent->setFollowUpMode(mode);
        m_config.settings.setGlobal("followUpMode", mode == QueueMode::All ? "all" : "one-at-a-time");
    }

    Result<CompactionResult> compact(const std::optional<std::string>& customInstructions) override {
        abort();
        waitForIdle();
        auto result = m_compaction->compactManual(customInstructions);
        notifyIdle();
        return result;
    }

    void abortCompaction() override {
        m_compaction->abort();
    }

    void abortBranchSummary() override {
        m_navigator->abort();
    }

    void setAutoCompactionEnabled(bool enabled) override {
        m_config.settings.setGlobalNested("compaction", "enabled", enabled);
    }

    bool autoCompactionEnabled() const override {
        return m_config.settings.view().compactionEnabled();
    }

    void setAutoRetryEnabled(bool enabled) override {
        m_config.settings.setGlobalNested("retry", "enabled", enabled);
    }

    bool autoRetryEnabled() const override {
        return m_config.settings.view().retryEnabled();
    }

    void abortRetry() override {
        m_retry->abort();
    }

    Result<BashResult> executeBash(const std::string& command, const std::function<void(const std::string&)>& onChunk, bool excludeFromContext, const std::optional<std::string>& id) override {
        return m_bash->execute(command, onChunk, excludeFromContext, id);
    }

    void recordBashResult(const std::string& command, const BashResult& result, bool excludeFromContext) override {
        m_bash->record(command, result, excludeFromContext);
    }

    void abortBash() override {
        m_bash->abort();
    }

    bool isBashRunning() const override {
        return m_bash->running();
    }

    std::vector<std::string> activeToolNames() const override {
        return m_loadout->activeToolNames();
    }

    std::vector<ToolInfo> allTools() const override {
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

    void setActiveToolsByName(const std::vector<std::string>& names) override {
        m_loadout->setActiveTools(names);
    }

    std::vector<SlashCommandInfo> slashCommands() const override {
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

    std::vector<SessionEntry> entries() const override {
        return m_config.session.entries();
    }

    std::optional<std::string> leafId() const override {
        return m_config.session.leafId();
    }

    std::vector<SessionTreeNode> tree() const override {
        return m_config.session.tree();
    }

    void setSessionName(const std::string& name) override {
        m_config.session.appendSessionInfo(name);
        AgentSessionEvent event;
        event.type = SessionEventType::SessionInfoChanged;
        event.name = m_config.session.sessionName();
        m_hub.emit(event);
    }

    Result<NavigateTreeResult> navigateTree(const std::string& targetId, const NavigateTreeOptions& options) override {
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

    std::vector<ForkableMessage> forkableMessages() const override {
        return m_navigator->forkableMessages();
    }

    Result<void> reload() override {
        m_config.settings.reload();
        m_agent->setSteeringMode(queueMode(m_config.settings.view().steeringMode()));
        m_agent->setFollowUpMode(queueMode(m_config.settings.view().followUpMode()));
        if (auto loaded = m_config.resources.reload(); !loaded) {
            return loaded;
        }
        m_loadout->rebuild();
        return {};
    }

    void dispose() override {
        if (m_agent == nullptr) {
            return;
        }
        abort();
        m_bash->abort();
        m_agent->unsubscribe(m_agentListener);
        m_hub.clear();
    }

private:
    // Construction
    void createAgent() {
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
        if (m_hooks) {
            options.loopConfig.beforeToolCall = [this](const ToolCallContext& context, const std::shared_ptr<AbortSignal>&) {
                return m_hooks->beforeToolCall(context);
            };
            options.loopConfig.afterToolCall = [this](const ToolCallContext& context, const std::shared_ptr<AbortSignal>&) {
                return m_hooks->afterToolCall(context);
            };
            options.loopConfig.transformContext = [this](const std::vector<AgentMessage>& messages,
                                                         const std::shared_ptr<AbortSignal>&) {
                return m_hooks->transformContext(messages);
            };
        }
        m_agent = m_config.agents.create(std::move(options));
    }

    void createCollaborators() {
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

    StreamOptions streamOptions() const {
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

    std::optional<AgentLoopTurnUpdate> prepareRequest(const PrepareRequestContext& request) const {
        AgentLoopTurnUpdate update;
        AgentContext context;
        context.messages = m_config.session.buildSessionProjection().messages;
        if (request.context != nullptr) {
            context.tools = request.context->tools;
        }
        update.context = std::move(context);
        return update;
    }

    std::optional<AgentLoopTurnUpdate> prepareNextTurn(const AgentTurnContext&) {
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

    // Agent events
    void onAgentEvent(const AgentEvent& event) {
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

    void onMessageStart(const AgentEvent& event) {
        if (event.message == nullptr) {
            return;
        }
        if (const auto* user = std::get_if<UserMessage>(event.message.get())) {
            m_postRun->resetOverflowRecovery();
            m_pending->delivered(userText(*user));
        }
    }

    void onMessageEnd(const AgentEvent& event) {
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

    void emitAgentEvent(const AgentEvent& event) {
        AgentSessionEvent out;
        out.type = event.type == AgentEventType::AgentEnd ? SessionEventType::AgentEnd : SessionEventType::Agent;
        out.agent = std::make_shared<const AgentEvent>(event);
        if (event.type == AgentEventType::AgentEnd) {
            out.willRetry = m_retry->willRetryAfterAgentEnd(event.messages, m_agent->model().contextWindow,
                                                            abortRequested());
        }
        m_hub.emit(out);
    }

    void emitSettled() {
        m_runActive = false;
        AgentSessionEvent event;
        event.type = SessionEventType::AgentSettled;
        m_hub.emit(event);
        notifyIdle();
    }

    std::string userText(const UserMessage& message) const {
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

    // Prompt flow
    Result<PromptDisposition> queueWhileStreaming(const std::string& text, const PromptOptions& options) {
        if (options.streamingBehavior == StreamingBehavior::None) {
            return std::unexpected(Error{"agent_busy",
                                         "Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message."});
        }
        return queueInput(text, options.images, options.streamingBehavior == StreamingBehavior::Steer);
    }

    Result<void> ensureReady() {
        const Model model = m_agent->model();
        if (model.id.empty()) {
            return std::unexpected(Error{"no_model", m_guidance.noModelSelected()});
        }
        if (!m_config.models.hasConfiguredAuth(model.provider)) {
            return std::unexpected(Error{"no_auth", m_guidance.noApiKeyFound(model.provider)});
        }
        return {};
    }

    std::vector<AgentMessage> buildPromptMessages(const std::string& text, const std::vector<ImageContent>& images) {
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

    UserMessage userMessage(const std::string& text, const std::vector<ImageContent>& images) const {
        UserMessage message;
        std::vector<UserContentBlock> blocks{TextContent{text, std::nullopt}};
        for (const auto& image : images) {
            blocks.emplace_back(image);
        }
        message.content = std::move(blocks);
        message.timestamp = m_config.clock.nowMs();
        return message;
    }

    Result<void> runAgentPrompt(std::vector<AgentMessage> messages) {
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

    void continueUntilSettled() {
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

    std::string expand(const std::string& text) const {
        const auto skill = m_skills.expand(text, m_config.resources.resources().skills);
        const std::string expanded = skill ? *skill : text;
        return m_templates.expand(expanded, m_config.resources.resources().promptTemplates);
    }

    Result<PromptDisposition> queueInput(const std::string& text, const std::vector<ImageContent>& images, bool steering) {
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

    CustomMessage customMessage(const std::string& customType, const Json& content, bool display, const Json& details) const {
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

    void flushPending() {
        m_bash->flushPending();
        m_custom->flush();
    }

    // Helpers
    bool abortRequested() const {
        return m_abortRequested.load();
    }

    bool busy() const {
        return !isIdle();
    }

    void notifyIdle() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
        }
        m_idle.notify_all();
    }

    QueueMode queueMode(const std::string& name) const {
        return name == "all" ? QueueMode::All : QueueMode::OneAtATime;
    }

    AgentSessionConfig m_config;
    AgentMessageConverter m_converter;
    AuthGuidance m_guidance;
    SessionEventHub m_hub;
    SessionEventCodec m_eventCodec;
    std::unique_ptr<PluginHookDispatcher> m_hooks;
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

// ---------------------------------------------------------------------------
// Agent events
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Idle tracking
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Prompting
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Subscription and state
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Model, thinking and queue modes
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Compaction and retry
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Bash, tools and the session tree
// ---------------------------------------------------------------------------
