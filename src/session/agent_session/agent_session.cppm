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
import pi.support.agent_message_codec;
import pi.support.branch_summarizer;
import pi.support.bug_report_coordinator;
import pi.support.cache_warmer;
import pi.support.compaction_controller;
import pi.support.compactor;
import pi.support.context_usage_calculator;
import pi.support.custom_message_queue;
import pi.support.install_telemetry_policy;
import pi.support.model_codec;
import pi.support.model_controller;
import pi.support.pending_input_tracker;
import pi.support.plugin_hook_dispatcher;
import pi.support.plugin_session_events;
import pi.support.post_run_handler;
import pi.support.prompt_loadout;
import pi.support.prompt_template_expander;
import pi.support.provider_attribution;
import pi.support.recovery_attempt_omitter;
import pi.support.thinking_level_resolver;
import pi.support.session_bash_controller;
import pi.support.session_context_refresher;
import pi.support.session_event_codec;
import pi.support.session_event_hub;
import pi.support.session_message_persister;
import pi.support.session_stats_calculator;
import pi.support.skill_command_expander;
import pi.support.summary_generator;
import pi.support.virtual_model_names;
import pi.support.error_stream_factory;
import pi.support.html_exporter;
import pi.support.path_resolver;
import pi.support.session_export_data;
import pi.support.transcript_normalizer;

/**
 * IAgentSession over an Agent and a session tree: persists every finished message, keeps the
 * agent's context equal to the tree's projection, retries and compacts as needed and exposes the
 * model, tool, bash and tree operations. The behavior of the TypeScript AgentSession; plugins attach
 * through the hook bus: the agent loop's hook points (PluginHookDispatcher) and the events around the
 * session (PluginSessionEvents: `input`, `before_agent_start`, `before_provider_request`, compaction and
 * tree events). Not ported: extension commands and the mutable `systemPromptOptions` of
 * `before_agent_start` (plugins see the rendered prompt and may replace it for the turn).
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
            m_events = std::make_unique<PluginSessionEvents>(*m_config.hooks);
        }
        createAgent();
        createCollaborators();
        createCacheWarmer();
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
        const Model current = limitsModel(nullptr);
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
        if (options.expandPromptTemplates) {
            if (const auto command = runPluginCommand(text)) {
                if (!*command) {
                    return std::unexpected(command->error());
                }
                if (options.onDisposition) {
                    options.onDisposition(PromptDisposition::Handled);
                }
                return PromptDisposition::Handled;
            }
        }
        InputOutcome input;
        input.text = text;
        input.images = options.images;
        if (m_events) {
            input = m_events->input(text, options.images, options.source, streamingName(options.streamingBehavior));
            if (input.handled) {
                if (options.onDisposition) {
                    options.onDisposition(PromptDisposition::Handled);
                }
                return PromptDisposition::Handled;
            }
        }
        PromptOptions effective = options;
        effective.images = input.images;
        const std::string expanded = options.expandPromptTemplates ? expand(input.text) : input.text;
        if (isStreaming()) {
            auto queued = queueWhileStreaming(expanded, effective);
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
        std::vector<AgentMessage> messages = buildPromptMessages(expanded, effective.images);
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
        abortPluginCommand();
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
        if (m_events) {
            if (auto supplied = m_events->userBash(command, excludeFromContext, m_config.session.cwd())) {
                m_bash->record(command, *supplied, excludeFromContext);
                return *supplied;
            }
        }
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
        if (m_config.commands != nullptr) {
            for (const PluginCommandInfo& command : m_config.commands->commands()) {
                out.push_back(SlashCommandInfo{command.name, command.description, "extension", SourceInfo{command.path, "extension", "temporary", "top-level", std::nullopt}});
            }
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

    Result<std::string> exportHtml(const std::optional<std::string>& outputPath, const std::string& theme) override {
        if (m_config.exportAssets == nullptr || m_config.base64 == nullptr) {
            return std::unexpected(Error{"unsupported", "HTML export is not available in this host"});
        }
        const std::optional<std::string> file = m_config.session.sessionFile();
        if (!file || !m_config.session.isPersisted()) {
            return std::unexpected(Error{"export_failed", "Cannot export in-memory session to HTML"});
        }
        if (!m_config.files.exists(*file)) {
            return std::unexpected(Error{"export_failed", "Nothing to export yet - start a conversation first"});
        }
        Json tools = Json::array();
        for (const ToolInfo& tool : allTools()) {
            if (tool.active) {
                tools.push_back(Json{{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}});
            }
        }
        const Json data = SessionExportData().build(m_config.session, systemPrompt(), tools);
        auto html = HtmlExporter(m_config.exportAssets->assets(), *m_config.base64).render(data, theme);
        if (!html) {
            return std::unexpected(html.error());
        }
        const PathResolver paths(m_config.files.homeDirectory());
        const std::string name = "pi-session-" + std::filesystem::path(*file).stem().string() + ".html";
        const std::string target = paths.resolveToCwd(outputPath.value_or(name), m_config.cwd);
        if (auto written = m_config.files.writeFile(target, *html); !written) {
            return std::unexpected(written.error());
        }
        return target;
    }

    Result<Json> reportBug(const Json& options) override {
        if (m_config.system == nullptr || m_config.http == nullptr || m_config.environment == nullptr) {
            return std::unexpected(Error{"unsupported", "Bug reports are not available in this host"});
        }
        BugReportRequest request;
        if (options.is_object()) {
            if (options.contains("hint") && options["hint"].is_string()) {
                request.hint = options["hint"].get<std::string>();
            }
            request.includeSession = options.contains("includeSession") && options["includeSession"] == true;
            request.includeSummary = options.contains("includeSummary") && options["includeSummary"] == true;
            request.delivery = options.value("delivery", std::string("zip"));
            if (options.contains("outputPath") && options["outputPath"].is_string()) {
                request.outputPath = options["outputPath"].get<std::string>();
            }
        }
        const Model model = m_agent->model();
        BugReportBuilder builder(*m_config.environment, *m_config.system, m_config.clock, m_config.ids);
        BugReportInput input;
        input.sessionId = m_config.session.sessionId();
        input.cwd = m_config.cwd;
        input.messageCount = static_cast<int>(m_agent->messages().size());
        if (!model.id.empty()) {
            input.model = model;
            input.provider = builder.describeProvider(m_config.models, model.provider, model.baseUrl);
        }
        input.thinkingLevel = m_levels.levelName(m_agent->thinkingLevel());
        if (m_config.plugins) {
            input.plugins = m_config.plugins();
        }
        input.globalSettings = m_config.settings.globalSettings();
        input.projectSettings = m_config.settings.projectSettings();
        SummarizationOptions summary;
        if (request.includeSummary) {
            auto chosen = summaryModel();
            if (!chosen) {
                return std::unexpected(chosen.error());
            }
            summary.model = chosen->model;
            summary.thinkingLevel = chosen->thinkingLevel.value_or(ThinkingLevel::Off);
            summary.stream.sessionId = m_config.session.sessionId();
            summary.streamFn = [this](const Model& target, const TranscriptContext& context, const StreamOptions& stream) { return m_config.models.stream(target, context, stream); };
            summary.retry = m_config.settings.view().retryPolicy();
        }
        BugReportSummarizer summarizer(m_generator);
        SessionBranchSerializer serializer(m_config.clock);
        BugReportUploader uploader(*m_config.http, m_config.ids);
        CrashLog crashes(m_config.files, m_config.clock);
        BugReportCoordinator coordinator(builder, summarizer, serializer, uploader, crashes, m_config.files, m_config.clock, *m_config.environment);
        const RadiusGateway radius;
        const auto token = [this, &radius]() -> std::optional<std::string> {
            const auto auth = m_config.models.getAuth(radius.providerId());
            return auth && auth->has_value() ? (*auth)->auth.apiKey : std::nullopt;
        };
        auto outcome = coordinator.report(request, std::move(input), m_agent->messages(), summary, m_config.session, token, m_config.agentDir, m_config.cwd, radius.gatewayUrl(*m_config.environment));
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        Json out = Json::object({{"id", outcome->id}, {"delivery", outcome->delivery}});
        if (outcome->path) {
            out["path"] = *outcome->path;
        }
        return out;
    }

    CacheWarmingStatus cacheWarmingStatus() const override {
        if (!m_warmer) {
            CacheWarmingStatus status;
            status.reason = "cache warming unavailable";
            return status;
        }
        return m_warmer->status();
    }

    Result<void> setCacheWarmingMode(const std::string& mode) override {
        if (mode != "off" && mode != "streaming" && mode != "idle") {
            return std::unexpected(Error{"invalid_argument", "Cache warming mode must be off, streaming or idle"});
        }
        if (auto stored = m_config.settings.setGlobal("cacheWarming", mode); !stored) {
            return stored;
        }
        if (m_warmer) {
            m_warmer->onModeChanged();
        }
        return {};
    }

    void dispose() override {
        if (m_agent == nullptr) {
            return;
        }
        if (m_warmer) {
            m_warmer->setOnWarmed({});
            m_warmer->cancel();
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
            return streamRouted(model, context, request);
        };
        options.loopConfig.options = streamOptions();
        options.loopConfig.convertToLlm = [this](const std::vector<AgentMessage>& messages) {
            return m_converter.convert(messages);
        };
        options.loopConfig.prepareRequest = [this](const PrepareRequestContext& request,
                                                   const std::shared_ptr<AbortSignal>& signal) { return prepareRequest(request, signal); };
        options.loopConfig.prepareNextTurn = [this](const AgentTurnContext& turn) { return prepareNextTurn(turn); };
        if (m_hooks) {
            options.loopConfig.beforeToolCall = [this](const ToolCallContext& context, const std::shared_ptr<AbortSignal>&) {
                return m_hooks->beforeToolCall(context);
            };
            options.loopConfig.afterToolCall = [this](const ToolCallContext& context, const std::shared_ptr<AbortSignal>&) {
                return m_hooks->afterToolCall(context);
            };
        }
        options.loopConfig.transformContext = [this](const std::vector<AgentMessage>& messages,
                                                     const std::shared_ptr<AbortSignal>&) {
            return projectForcedPrompt(m_hooks ? m_hooks->transformContext(messages) : messages);
        };
        m_agent = m_config.agents.create(std::move(options));
    }

    /** The prompt-cache warmer exists only when the session was given an environment to read PI_CACHE_RETENTION from. */
    void createCacheWarmer() {
        if (m_config.environment == nullptr) {
            return;
        }
        CacheWarmer::Decide decide;
        if (m_events) {
            decide = [this](const CacheWarmingDecision& decision) {
                return m_events->cacheWarmingDecision(decision.warmCost, decision.missCost, decision.continuationProbability, decision.action);
            };
        }
        m_warmer = std::make_unique<CacheWarmer>(m_config.models, m_config.session, m_config.sleeper, m_config.clock, *m_config.environment,
                                                 [this] { return m_config.settings.view().cacheWarmingMode(); }, std::move(decide));
        m_warmer->setOnWarmed([this](const SessionEntry& entry) {
            AgentSessionEvent event;
            event.type = SessionEventType::EntryAppended;
            event.entry = entry;
            m_hub.emit(event);
        });
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
        if (m_events) {
            m_models->setObservers(
                [this](const Model& model, const Model& previous, const std::string& source) {
                    m_events->modelSelected(m_modelCodec.toJson(model), previous.id.empty() ? Json() : m_modelCodec.toJson(previous), source);
                },
                [this](ThinkingLevel level, ThinkingLevel previous) { m_events->thinkingLevelSelected(m_levels.levelName(level), m_levels.levelName(previous)); });
        }
        m_loadout = std::make_unique<PromptLoadout>(*m_agent, session, m_config.tools, m_config.resources,
                                                    m_config.clock, m_config.cwd);
        m_loadout->setToolFilter(m_config.allowedTools, m_config.excludedTools);
        m_compaction->setEvents(m_events.get());
        m_navigator->setEvents(m_events.get());
        m_compaction->setSummaryModel([this] { return summaryModel(); });
        m_navigator->setSummaryModel([this] { return summaryModel(); });
        m_pending = std::make_unique<PendingInputTracker>(m_hub);
        m_custom = std::make_unique<CustomMessageQueue>(session, *m_refresher, m_hub);
        m_persister = std::make_unique<SessionMessagePersister>(session);
        m_postRun = std::make_unique<PostRunHandler>(*m_agent, session, settings, *m_retry, *m_compaction, *m_omitter, m_hub);
        m_postRun->setLimitsSource([this](const AssistantMessage* message) { return limitsModel(message); });
    }

    /**
     * While a `before_agent_start` plugin replaced the system prompt for the turn, the request carries that text as its
     * only system message; the tool declarations of the transcript's current system message stay.
     */
    std::vector<AgentMessage> projectForcedPrompt(std::vector<AgentMessage> messages) {
        std::optional<std::string> forced;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            forced = m_forcedPrompt;
        }
        if (!forced) {
            return messages;
        }
        std::vector<Message> system;
        std::vector<AgentMessage> rest;
        for (auto& message : messages) {
            if (const auto* entry = std::get_if<SystemMessage>(&message)) {
                system.emplace_back(*entry);
            } else {
                rest.push_back(std::move(message));
            }
        }
        const auto current = m_transcript.currentSystemMessage(system);
        SystemMessage head;
        head.content = *forced;
        if (current) {
            head.toolsAdded = current->toolsAdded;
            head.timestamp = current->timestamp;
        } else {
            head.timestamp = m_config.clock.nowMs();
        }
        std::vector<AgentMessage> out;
        out.emplace_back(std::move(head));
        out.insert(out.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
        return out;
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
        if (m_events) {
            options.onPayload = [this](const Json& payload, const Model&) { return m_events->beforeProviderRequest(payload); };
            if (m_config.hooks->hasHandlers("after_provider_response")) {
                options.onResponse = [this](const ProviderResponse& response, const Model&) {
                    Json headers = Json::object();
                    for (const auto& [name, value] : response.headers) {
                        headers[name] = value;
                    }
                    m_events->afterProviderResponse(response.status, headers);
                };
            }
            if (m_config.hooks->hasHandlers("provider_stream_event")) {
                options.onStreamEvent = [this](const Json& data, const Model& model) { m_events->providerStreamEvent(model.provider, model.api, model.id, data); };
            }
        }
        options.transformHeaders = [this](const Model& model, const ProviderAttribution::Headers& headers) { return transformHeaders(model, headers); };
        return options;
    }

    /** Adds the attribution headers, then lets plugins change the assembled headers (`before_provider_headers`). */
    ProviderAttribution::Headers transformHeaders(const Model& model, const ProviderAttribution::Headers& headers) const {
        const bool telemetry = m_installTelemetry.enabled(m_config.settings.view(), m_config.telemetryEnv);
        ProviderAttribution::Headers merged = m_attribution.merge(model, telemetry, m_config.session.sessionId(), {headers});
        if (!m_events) {
            return merged;
        }
        Json asJson = Json::object();
        for (const auto& [name, value] : merged) {
            if (value) {
                asJson[name] = *value;
            }
        }
        const Json changed = m_events->beforeProviderHeaders(asJson);
        ProviderAttribution::Headers out;
        for (const auto& entry : changed.items()) {
            if (entry.value().is_string()) {
                out.emplace_back(entry.key(), entry.value().get<std::string>());
            }
        }
        return out;
    }

    std::optional<AgentLoopTurnUpdate> prepareRequest(const PrepareRequestContext& request, const std::shared_ptr<AbortSignal>& signal) {
        AgentLoopTurnUpdate update;
        AgentContext context;
        context.messages = m_config.session.buildSessionProjection().messages;
        if (request.context != nullptr) {
            context.tools = request.context->tools;
        }
        const std::vector<AgentMessage> sent = context.messages;
        update.context = std::move(context);
        const Model selected = m_agent->model();
        if (m_virtual.isVirtual(selected)) {
            routeRequest(selected, sent, signal, update);
        }
        return update;
    }

    /**
     * Routes a request of the selected virtual model: the loop streams the physical model the router picked for this request and
     * records its thinking level; the selection stays on the agent. A failed route leaves the virtual model in the update, and
     * the stream function ends that request with the routing error.
     */
    void routeRequest(const Model& selected, const std::vector<AgentMessage>& messages, const std::shared_ptr<AbortSignal>& signal, AgentLoopTurnUpdate& update) {
        VirtualResolveRequest resolve;
        resolve.model = selected;
        resolve.thinkingLevel = m_agent->thinkingLevel();
        resolve.messages = m_converter.convert(messages);
        resolve.signal = signal;
        resolve.state = virtualModelState(selected);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            resolve.failed = std::exchange(m_failedResponse, std::nullopt);
        }
        resolve.reason = resolve.failed ? "retry" : startsWithUserTurn(messages) ? "user" : "continuation";
        auto route = m_config.models.resolveVirtual(resolve);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_routeError = route ? std::nullopt : std::optional<std::string>(route.error().message);
        }
        if (!route) {
            return;
        }
        if (route->state) {
            Json data = Json::object({{"provider", selected.provider}, {"modelId", selected.id}, {"state", *route->state}});
            (void)m_config.session.appendCustomEntry(m_virtual.stateEntryType(), data);
        }
        update.model = route->model;
        update.thinkingLevel = route->thinkingLevel;
    }

    /** Only messages the user wrote start a turn; custom messages can follow them. */
    bool startsWithUserTurn(const std::vector<AgentMessage>& messages) const {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (std::holds_alternative<AssistantMessage>(*it)) {
                return false;
            }
            if (std::holds_alternative<UserMessage>(*it)) {
                return true;
            }
        }
        return true;
    }

    /** The router state last stored on this branch for the virtual model; null before the first. */
    Json virtualModelState(const Model& model) const {
        const std::vector<SessionEntry> branch = m_config.session.branchPath();
        for (auto it = branch.rbegin(); it != branch.rend(); ++it) {
            if (it->type != "custom" || it->body.value("customType", "") != m_virtual.stateEntryType() || !it->body.contains("data") || !it->body["data"].is_object()) {
                continue;
            }
            const Json& data = it->body["data"];
            if (data.value("provider", "") == model.provider && data.value("modelId", "") == model.id) {
                return data.contains("state") ? data["state"] : Json();
            }
        }
        return Json();
    }

    /** Streams a request: virtual models never reach a provider, so an unrouted one ends with the routing error. */
    std::shared_ptr<AssistantMessageStream> streamRouted(const Model& model, const TranscriptContext& original, const StreamOptions& request) {
        // Plugins see the final transcript of the session's own requests only, not compaction and summary calls.
        const bool sessionRequest = request.sessionId && *request.sessionId == m_config.session.sessionId();
        const TranscriptContext context = m_hooks && sessionRequest && !m_virtual.isVirtual(model) ? m_hooks->transformFinalContext(original) : original;
        if (m_virtual.isVirtual(model)) {
            std::optional<std::string> reason;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                reason = m_routeError;
            }
            return m_errors.failed(model, reason.value_or("Virtual model " + model.provider + "/" + model.id + " must be routed before streaming"), m_config.clock.nowMs());
        }
        // Compaction and summaries use routing ids of their own; only session requests replace the cache entry, so only they
        // restart the warming. It goes on while the transcript still extends the request's prefix.
        if (m_warmer && sessionRequest) {
            CacheWarmRequest warm;
            warm.model = model;
            warm.context = context;
            warm.options = request;
            m_warmer->start(std::move(warm), cacheContextIsCurrent(model));
        }
        return m_config.models.stream(model, context, request);
    }

    /** True while the selected model is the request's and the transcript still starts with the messages the request was sent with. */
    std::function<bool()> cacheContextIsCurrent(const Model& requestModel) const {
        std::vector<Json> sent;
        for (const AgentMessage& message : m_agent->messages()) {
            sent.push_back(m_messageCodec.toJson(message));
        }
        return [this, provider = requestModel.provider, id = requestModel.id, sent = std::move(sent)] {
            const Model current = m_agent->model();
            if (current.provider != provider || current.id != id) {
                return false;
            }
            const std::vector<AgentMessage> messages = m_agent->messages();
            if (sent.size() > messages.size()) {
                return false;
            }
            for (std::size_t i = 0; i < sent.size(); ++i) {
                if (m_messageCodec.toJson(messages[i]) != sent[i]) {
                    return false;
                }
            }
            return true;
        };
    }

    /** The physical model of the selected virtual model's latest response (or the selected model itself) for limits. */
    Model limitsModel(const AssistantMessage* message) const {
        const Model selected = m_agent->model();
        if (!m_virtual.isVirtual(selected)) {
            return selected;
        }
        const std::vector<AgentMessage> all = m_agent->messages();
        const AssistantMessage* source = message;
        if (source == nullptr) {
            for (auto it = all.rbegin(); it != all.rend() && source == nullptr; ++it) {
                const auto* assistant = std::get_if<AssistantMessage>(&*it);
                if (assistant != nullptr && assistant->stopReason != StopReason::Error && assistant->stopReason != StopReason::Aborted) {
                    source = assistant;
                }
            }
        }
        if (source != nullptr) {
            if (auto physical = m_config.models.physicalModel(source->provider, source->model)) {
                return *physical;
            }
        }
        return selected;
    }

    /** Routes a request made outside the agent loop (a compaction or branch summary); the physical model sizes and answers it. */
    Result<RoutedSelection> summaryModel() {
        const Model selected = m_agent->model();
        if (!m_virtual.isVirtual(selected)) {
            return RoutedSelection{selected, m_agent->thinkingLevel()};
        }
        VirtualResolveRequest resolve;
        resolve.model = selected;
        resolve.thinkingLevel = m_agent->thinkingLevel();
        resolve.reason = "direct";
        resolve.messages = m_converter.convert(m_agent->messages());
        auto route = m_config.models.resolveVirtual(resolve);
        if (!route) {
            return std::unexpected(route.error());
        }
        return RoutedSelection{route->model, route->thinkingLevel};
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
            out.willRetry = m_retry->willRetryAfterAgentEnd(event.messages, limitsModel(nullptr).contextWindow,
                                                            abortRequested());
        }
        m_hub.emit(out);
    }

    void emitSettled() {
        m_runActive = false;
        AgentSessionEvent event;
        event.type = SessionEventType::AgentSettled;
        m_hub.emit(event);
        if (m_warmer) {
            m_warmer->onAgentSettled();
        }
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
    std::string streamingName(StreamingBehavior behavior) const {
        if (!isStreaming() || behavior == StreamingBehavior::None) {
            return "";
        }
        return behavior == StreamingBehavior::Steer ? "steer" : "followUp";
    }

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
        AgentStartOutcome start;
        if (m_events) {
            start = m_events->beforeAgentStart(text, images, m_loadout->systemPromptText());
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_forcedPrompt = start.systemPrompt;
        }
        std::vector<AgentMessage> messages;
        messages.emplace_back(userMessage(text, images));
        for (auto& queued : m_custom->takeNextTurn()) {
            messages.emplace_back(std::move(queued));
        }
        for (const Json& added : start.messages) {
            messages.emplace_back(customMessage(added["customType"].get<std::string>(), added["content"],
                                                added["display"].get<bool>(), added["details"]));
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
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_failedResponse.reset();
            m_routeError.reset();
        }
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
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_forcedPrompt.reset();
        }
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
            if (last && last->stopReason == StopReason::Error) {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_failedResponse = last;
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

    /** `/name args` naming a plugin command runs it instead of reaching the model, even while the agent streams; nullopt when no plugin command is named. */
    std::optional<Result<void>> runPluginCommand(const std::string& text) {
        if (m_config.commands == nullptr || !text.starts_with("/")) {
            return std::nullopt;
        }
        const std::size_t space = text.find(' ');
        const std::string name = text.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        const std::string args = space == std::string::npos ? std::string() : text.substr(space + 1);
        const auto signal = std::make_shared<AbortSignal>();
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandAbort = signal;
        }
        const auto outcome = m_config.commands->execute(name, args, signal);
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandAbort = nullptr;
        }
        return outcome;
    }

    void abortPluginCommand() {
        std::shared_ptr<AbortSignal> signal;
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            signal = m_commandAbort;
        }
        if (signal) {
            signal->abort();
        }
    }

    AgentSessionConfig m_config;
    AgentMessageConverter m_converter;
    AuthGuidance m_guidance;
    SessionEventHub m_hub;
    SessionEventCodec m_eventCodec;
    std::unique_ptr<PluginHookDispatcher> m_hooks;
    std::unique_ptr<PluginSessionEvents> m_events;
    std::unique_ptr<CacheWarmer> m_warmer;
    AgentMessageCodec m_messageCodec;
    ThinkingLevelResolver m_levels;
    ModelCodec m_modelCodec;
    ProviderAttribution m_attribution;
    InstallTelemetryPolicy m_installTelemetry;
    TranscriptNormalizer m_transcript;
    VirtualModelNames m_virtual;
    ErrorStreamFactory m_errors;
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
    std::mutex m_commandMutex;
    std::shared_ptr<AbortSignal> m_commandAbort;
    std::atomic<bool> m_abortRequested{false};
    std::mutex m_mutex;
    std::condition_variable m_idle;
    std::optional<AssistantMessage> m_lastAssistant;
    std::vector<ToolResultMessage> m_lastToolResults;
    std::optional<std::string> m_forcedPrompt;
    std::optional<AssistantMessage> m_failedResponse;
    std::optional<std::string> m_routeError;
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
