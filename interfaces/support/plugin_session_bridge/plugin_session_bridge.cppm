export module pi.support.plugin_session_bridge;

import std;
export import pi.plugin.i_plugin_session_bridge;
export import pi.provider.i_model_runtime;
export import pi.session.i_agent_session;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
import pi.support.model_codec;
import pi.support.thinking_level_resolver;

/**
 * Carries out the operations plugins ask of their session (PiHostApi.session_call; see pi_plugin.h for the method list), over the
 * live session, its session tree, the settings and the model runtime. Replacing the session (new session, switching, forking) is
 * not offered: it would dispose the plugin host while the plugin's handler is still on the stack. A compaction a plugin starts
 * runs on a thread of its own so a handler on the agent's thread cannot wait on itself; the destructor waits for it.
 */
export class PluginSessionBridge : public IPluginSessionBridge {
public:
    /** Reports the state of the MCP servers (`getMcpServers`); may be empty. */
    using McpServers = std::function<Json()>;
    /** Asks the host to end the session (`shutdown`). */
    using Shutdown = std::function<void()>;

    PluginSessionBridge(IAgentSession& session, ISessionManager& sessions, ISettingsManager& settings, IModelRuntime& models, McpServers mcpServers, Shutdown shutdown)
        : m_session(session),
          m_sessions(sessions),
          m_settings(settings),
          m_models(models),
          m_mcpServers(std::move(mcpServers)),
          m_shutdown(std::move(shutdown)) {}

    ~PluginSessionBridge() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_compactions == 0; });
    }

    PluginSessionBridge(const PluginSessionBridge&) = delete;
    PluginSessionBridge& operator=(const PluginSessionBridge&) = delete;

    Json call(const std::string& method, const Json& params, const AbortSignal* abort) override {
        if (method.starts_with("sessionManager.")) {
            return sessionManager(method.substr(15), params);
        }
        if (method == "sendMessage" || method == "sendUserMessage" || method == "appendEntry" || method == "setLabel") {
            return messaging(method, params);
        }
        if (method == "getModel" || method == "setModel" || method == "getThinkingLevel" || method == "setThinkingLevel") {
            return models(method, params);
        }
        if (method == "newSession" || method == "switchSession" || method == "fork" || method == "navigateTree" || method == "reload" || method == "waitForIdle") {
            return control(method, params, abort);
        }
        return state(method, params);
    }

private:
    Json messaging(const std::string& method, const Json& params) {
        if (method == "appendEntry") {
            if (!text(params, "customType")) {
                return error("appendEntry needs a customType");
            }
            const auto id = m_sessions.appendCustomEntry(*text(params, "customType"), params.contains("data") ? params["data"] : Json(nullptr));
            return id ? Json{{"id", *id}} : error(id.error().message);
        }
        if (method == "setLabel") {
            if (!text(params, "entryId")) {
                return error("setLabel needs an entryId");
            }
            const auto id = m_sessions.appendLabelChange(*text(params, "entryId"), text(params, "label"));
            return id ? Json{{"id", *id}} : error(id.error().message);
        }
        if (method == "sendMessage") {
            return sendMessage(params);
        }
        const std::optional<std::string> input = text(params, "text");
        if (!input) {
            return error("sendUserMessage needs text");
        }
        const std::string deliverAs = text(params, "deliverAs").value_or("steer");
        if (m_session.isStreaming()) {
            const auto queued = deliverAs == "followUp" ? m_session.followUp(*input, {}) : m_session.steer(*input, {});
            return queued ? Json{{"ok", true}} : error(queued.error().message);
        }
        PromptOptions options;
        options.source = "extension";
        const auto started = m_session.prompt(*input, options);
        return started ? Json{{"ok", true}} : error(started.error().message);
    }

    Json sendMessage(const Json& params) {
        const auto customType = text(params, "customType");
        if (!customType || !params.contains("content")) {
            return error("sendMessage needs customType and content");
        }
        SendMessageOptions options;
        if (params.contains("triggerTurn") && params["triggerTurn"].is_boolean()) {
            options.triggerTurn = params["triggerTurn"].get<bool>();
        }
        const std::string deliverAs = text(params, "deliverAs").value_or("");
        options.deliverAs = deliverAs == "steer" ? DeliverAs::Steer : (deliverAs == "followUp" ? DeliverAs::FollowUp : (deliverAs == "nextTurn" ? DeliverAs::NextTurn : DeliverAs::Default));
        const bool display = !params.contains("display") || params["display"] != false;
        const auto sent = m_session.sendCustomMessage(*customType, params["content"], display, params.contains("details") ? params["details"] : Json(nullptr), options);
        return sent ? Json{{"ok", true}} : error(sent.error().message);
    }

    Json models(const std::string& method, const Json& params) {
        if (method == "getModel") {
            const Model model = m_session.model();
            return model.id.empty() ? Json(nullptr) : m_modelCodec.toJson(model);
        }
        if (method == "getThinkingLevel") {
            return Json{{"level", m_levels.levelName(m_session.thinkingLevel())}};
        }
        if (method == "setThinkingLevel") {
            const auto level = text(params, "level") ? m_levels.parseLevel(*text(params, "level")) : std::nullopt;
            if (!level) {
                return error("setThinkingLevel needs a known level");
            }
            m_session.setThinkingLevel(*level, true);
            return Json{{"ok", true}};
        }
        if (!text(params, "provider") || !text(params, "id")) {
            return error("setModel needs provider and id");
        }
        const auto model = m_models.find(*text(params, "provider"), *text(params, "id"));
        if (!model) {
            return Json{{"ok", false}};
        }
        const auto set = m_session.setModel(*model, true);
        return Json{{"ok", set.has_value()}};
    }

    Json state(const std::string& method, const Json& params) {
        if (method == "getSessionName") {
            const auto name = m_session.sessionName();
            return Json{{"name", name ? Json(*name) : Json(nullptr)}};
        }
        if (method == "setSessionName") {
            if (!text(params, "name")) {
                return error("setSessionName needs a name");
            }
            m_session.setSessionName(*text(params, "name"));
            return Json{{"ok", true}};
        }
        if (method == "getActiveTools") {
            return Json(m_session.activeToolNames());
        }
        if (method == "getAllTools") {
            Json out = Json::array();
            for (const ToolInfo& tool : m_session.allTools()) {
                out.push_back(Json{{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}, {"active", tool.active}});
            }
            return out;
        }
        if (method == "setActiveTools") {
            if (!params.contains("names") || !params["names"].is_array()) {
                return error("setActiveTools needs a names array");
            }
            std::vector<std::string> names;
            for (const Json& name : params["names"]) {
                if (name.is_string()) {
                    names.push_back(name.get<std::string>());
                }
            }
            m_session.setActiveToolsByName(names);
            return Json{{"ok", true}};
        }
        if (method == "getCommands") {
            Json out = Json::array();
            for (const SlashCommandInfo& command : m_session.slashCommands()) {
                out.push_back(Json{{"name", command.name}, {"description", command.description}, {"source", command.source}});
            }
            return out;
        }
        if (method == "getSettings") {
            return m_settings.settings();
        }
        if (method == "isIdle") {
            return Json{{"idle", m_session.isIdle()}};
        }
        if (method == "hasPendingMessages") {
            return Json{{"pending", m_session.pendingMessageCount() > 0}};
        }
        if (method == "isProjectTrusted") {
            return Json{{"trusted", m_settings.projectTrusted()}};
        }
        if (method == "abort") {
            m_session.abort();
            return Json{{"ok", true}};
        }
        if (method == "getContextUsage") {
            const auto usage = m_session.contextUsage();
            if (!usage) {
                return Json(nullptr);
            }
            return Json{{"tokens", usage->tokens ? Json(*usage->tokens) : Json(nullptr)}, {"contextWindow", usage->contextWindow}, {"percent", usage->percent ? Json(*usage->percent) : Json(nullptr)}};
        }
        if (method == "getSystemPrompt") {
            return Json{{"systemPrompt", m_session.systemPrompt()}};
        }
        if (method == "compact") {
            return compact(params);
        }
        if (method == "shutdown") {
            if (!m_shutdown) {
                return error("shutdown is not available in this host");
            }
            m_shutdown();
            return Json{{"ok", true}};
        }
        if (method == "getMcpServers") {
            return m_mcpServers ? m_mcpServers() : Json::array();
        }
        return error("unknown session method \"" + method + "\"");
    }

    Json compact(const Json& params) {
        const std::optional<std::string> instructions = text(params, "customInstructions");
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_compactions;
        }
        std::thread([this, instructions] {
            (void)m_session.compact(instructions);
            const std::lock_guard<std::mutex> lock(m_mutex);
            --m_compactions;
            m_changed.notify_all();
        }).detach();
        return Json{{"ok", true}};
    }

    Json control(const std::string& method, const Json& params, const AbortSignal* abort) {
        if (method == "newSession" || method == "switchSession" || method == "fork") {
            return error(method + " is not available to plugins");
        }
        if (method == "waitForIdle") {
            // Poll, so a cancelled call returns instead of waiting for a run that never ends.
            while (!m_session.isIdle() && (abort == nullptr || !abort->aborted())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return Json{{"ok", m_session.isIdle()}};
        }
        if (method == "reload") {
            const auto reloaded = m_session.reload();
            return reloaded ? Json{{"ok", true}} : error(reloaded.error().message);
        }
        if (method == "navigateTree") {
            if (!text(params, "targetId")) {
                return error("navigateTree needs a targetId");
            }
            NavigateTreeOptions options;
            options.summarize = params.contains("summarize") && params["summarize"] == true;
            options.customInstructions = text(params, "customInstructions");
            options.label = text(params, "label");
            const auto navigated = m_session.navigateTree(*text(params, "targetId"), options);
            return navigated ? Json{{"ok", true}} : error(navigated.error().message);
        }
        return error(method + " is not available to plugins");
    }

    Json sessionManager(const std::string& method, const Json& params) {
        if (method == "getEntries" || method == "getBranch") {
            Json out = Json::array();
            const std::vector<SessionEntry> entries = method == "getEntries" ? m_sessions.entries() : m_sessions.branchPath(text(params, "fromId"));
            for (const SessionEntry& entry : entries) {
                out.push_back(entry.body);
            }
            return out;
        }
        if (method == "getEntry") {
            const auto entry = text(params, "id") ? m_sessions.entry(*text(params, "id")) : std::nullopt;
            return entry ? entry->body : Json(nullptr);
        }
        if (method == "getLeafId") {
            const auto leaf = m_sessions.leafId();
            return Json{{"id", leaf ? Json(*leaf) : Json(nullptr)}};
        }
        if (method == "getSessionId") {
            return Json{{"id", m_sessions.sessionId()}};
        }
        if (method == "getCwd") {
            return Json{{"cwd", m_sessions.cwd()}};
        }
        return error("unknown session method \"sessionManager." + method + "\"");
    }

    std::optional<std::string> text(const Json& params, const std::string& key) const {
        if (params.is_object() && params.contains(key) && params[key].is_string()) {
            return params[key].get<std::string>();
        }
        return std::nullopt;
    }

    Json error(const std::string& message) const {
        return Json{{"error", message}};
    }

    IAgentSession& m_session;
    ISessionManager& m_sessions;
    ISettingsManager& m_settings;
    IModelRuntime& m_models;
    McpServers m_mcpServers;
    Shutdown m_shutdown;
    ModelCodec m_modelCodec;
    ThinkingLevelResolver m_levels;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_compactions = 0;
};
