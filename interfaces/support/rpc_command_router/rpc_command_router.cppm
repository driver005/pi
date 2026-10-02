module;

#include <nlohmann/json.hpp>

export module pi.support.rpc_command_router;

import std;
export import pi.provider.i_model_runtime;
export import pi.session.i_agent_session_runtime;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.support.model_codec;
export import pi.support.session_data_codec;
export import pi.support.session_event_codec;
export import pi.types.json;

/**
 * The JSONL command protocol of `pi rpc`: one JSON command in, a response (and the session's
 * events) out. Commands run on the caller's thread and block until done, so a transport runs each
 * command on its own worker; `prompt` answers as soon as the prompt is accepted. Port of
 * modes/rpc/rpc-mode.ts without extension UI requests.
 */
export class RpcCommandRouter {
public:
    using Output = std::function<void(const Json&)>;

    RpcCommandRouter(IAgentSessionRuntime& runtime, IModelRuntime& models, Output output);
    ~RpcCommandRouter();

    /** Starts streaming the current session's events to the output. */
    void attach();
    void detach();

    /** Runs one parsed command; responses go to the output (immediately or later for prompts). */
    void handle(const Json& command);

    /** The text of a command line that is not valid JSON: the error response to send. */
    Json parseError(const std::string& message) const;

private:
    using Handler = std::function<std::optional<Json>(const Json&)>;

    void registerPromptingCommands();
    void registerModelCommands();
    void registerSessionCommands();
    void registerStateCommands();

    Json success(const Json& command, const std::string& name, const std::optional<Json>& data = std::nullopt) const;
    Json failure(const Json& command, const std::string& name, const std::string& message) const;
    Json failure(const std::optional<std::string>& id, const std::string& name, const std::string& message) const;
    std::vector<ImageContent> images(const Json& command) const;
    std::string text(const Json& command, const char* key) const;
    bool flag(const Json& command, const char* key, bool fallback = false) const;
    std::string queueModeName(QueueMode mode) const;
    QueueMode parseQueueMode(const std::string& name) const;
    Json modelJson(const Model& model) const;
    Json replaced(const Json& command, const std::string& name, const Result<void>& outcome, Json data);

    std::optional<Json> prompt(const Json& command);
    std::optional<Json> queueInput(const Json& command, const std::string& name, bool steering);
    std::optional<Json> abort(const Json& command);
    std::optional<Json> getState(const Json& command);
    std::optional<Json> setModel(const Json& command);
    std::optional<Json> setThinkingLevel(const Json& command);
    std::optional<Json> compact(const Json& command);
    std::optional<Json> bash(const Json& command);
    std::optional<Json> fork(const Json& command, bool clone);
    std::optional<Json> getEntries(const Json& command);
    std::optional<Json> setSessionName(const Json& command);
    std::optional<Json> setMode(const Json& command, const std::string& name, bool steering);

    IAgentSessionRuntime& m_runtime;
    IModelRuntime& m_models;
    Output m_output;
    SessionEventCodec m_events;
    SessionDataCodec m_data;
    ModelCodec m_modelCodec;
    MessageCodec m_messages;
    AgentMessageCodec m_agentMessages;
    std::map<std::string, Handler> m_handlers;

    std::mutex m_mutex;
    IAgentSession* m_attached = nullptr;
    IAgentSession::ListenerId m_listener = 0;
};

RpcCommandRouter::RpcCommandRouter(IAgentSessionRuntime& runtime, IModelRuntime& models, Output output)
    : m_runtime(runtime), m_models(models), m_output(std::move(output)) {
    registerPromptingCommands();
    registerModelCommands();
    registerSessionCommands();
    registerStateCommands();
}

RpcCommandRouter::~RpcCommandRouter() {
    detach();
}

// ---------------------------------------------------------------------------
// Attachment
// ---------------------------------------------------------------------------

void RpcCommandRouter::attach() {
    detach();
    IAgentSession& session = m_runtime.session();
    const auto id = session.subscribe([this](const AgentSessionEvent& event) { m_output(m_events.toJson(event)); });
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_attached = &session;
    m_listener = id;
}

void RpcCommandRouter::detach() {
    IAgentSession* session = nullptr;
    IAgentSession::ListenerId id = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        session = std::exchange(m_attached, nullptr);
        id = m_listener;
    }
    if (session != nullptr) {
        session->unsubscribe(id);
    }
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

Json RpcCommandRouter::success(const Json& command, const std::string& name, const std::optional<Json>& data) const {
    Json out = Json::object();
    if (command.contains("id") && !command["id"].is_null()) {
        out["id"] = command["id"];
    }
    out["type"] = "response";
    out["command"] = name;
    out["success"] = true;
    if (data) {
        out["data"] = *data;
    }
    return out;
}

Json RpcCommandRouter::failure(const std::optional<std::string>& id, const std::string& name,
                               const std::string& message) const {
    Json out = Json::object();
    if (id) {
        out["id"] = *id;
    }
    out["type"] = "response";
    out["command"] = name;
    out["success"] = false;
    out["error"] = message;
    return out;
}

Json RpcCommandRouter::failure(const Json& command, const std::string& name, const std::string& message) const {
    std::optional<std::string> id;
    if (command.contains("id") && command["id"].is_string()) {
        id = command["id"].get<std::string>();
    }
    return failure(id, name, message);
}

Json RpcCommandRouter::parseError(const std::string& message) const {
    return failure(std::optional<std::string>(), "parse", "Failed to parse command: " + message);
}

std::string RpcCommandRouter::text(const Json& command, const char* key) const {
    const auto found = command.find(key);
    return found != command.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool RpcCommandRouter::flag(const Json& command, const char* key, bool fallback) const {
    const auto found = command.find(key);
    return found != command.end() && found->is_boolean() ? found->get<bool>() : fallback;
}

std::vector<ImageContent> RpcCommandRouter::images(const Json& command) const {
    std::vector<ImageContent> out;
    const auto found = command.find("images");
    if (found == command.end() || !found->is_array()) {
        return out;
    }
    for (const auto& item : *found) {
        if (item.is_object() && item.value("type", "") == "image") {
            out.push_back(ImageContent{item.value("data", ""), item.value("mimeType", "")});
        }
    }
    return out;
}

std::string RpcCommandRouter::queueModeName(QueueMode mode) const {
    return mode == QueueMode::All ? "all" : "one-at-a-time";
}

QueueMode RpcCommandRouter::parseQueueMode(const std::string& name) const {
    return name == "all" ? QueueMode::All : QueueMode::OneAtATime;
}

Json RpcCommandRouter::modelJson(const Model& model) const {
    return m_modelCodec.toJson(model);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void RpcCommandRouter::handle(const Json& command) {
    const std::string type = command.is_object() ? text(command, "type") : std::string();
    const auto handler = m_handlers.find(type);
    if (handler == m_handlers.end()) {
        m_output(failure(command.is_object() ? command : Json::object(), type, "Unknown command: " + type));
        return;
    }
    if (const auto response = handler->second(command)) {
        m_output(*response);
    }
}

void RpcCommandRouter::registerPromptingCommands() {
    m_handlers["prompt"] = [this](const Json& c) { return prompt(c); };
    m_handlers["steer"] = [this](const Json& c) { return queueInput(c, "steer", true); };
    m_handlers["follow_up"] = [this](const Json& c) { return queueInput(c, "follow_up", false); };
    m_handlers["abort"] = [this](const Json& c) { return abort(c); };
    m_handlers["clear_queue"] = [this](const Json& c) {
        return success(c, "clear_queue", m_data.queued(m_runtime.session().clearQueue()));
    };
    m_handlers["compact"] = [this](const Json& c) { return compact(c); };
    m_handlers["set_auto_compaction"] = [this](const Json& c) {
        m_runtime.session().setAutoCompactionEnabled(flag(c, "enabled"));
        return success(c, "set_auto_compaction");
    };
    m_handlers["set_auto_retry"] = [this](const Json& c) {
        m_runtime.session().setAutoRetryEnabled(flag(c, "enabled"));
        return success(c, "set_auto_retry");
    };
    m_handlers["abort_retry"] = [this](const Json& c) {
        m_runtime.session().abortRetry();
        return success(c, "abort_retry");
    };
    m_handlers["bash"] = [this](const Json& c) { return bash(c); };
    m_handlers["abort_bash"] = [this](const Json& c) {
        m_runtime.session().abortBash();
        return success(c, "abort_bash");
    };
}

void RpcCommandRouter::registerModelCommands() {
    m_handlers["set_model"] = [this](const Json& c) { return setModel(c); };
    m_handlers["cycle_model"] = [this](const Json& c) {
        const auto cycled = m_runtime.session().cycleModel(true, false);
        return success(c, "cycle_model", cycled ? m_data.modelCycle(*cycled) : Json(nullptr));
    };
    m_handlers["get_available_models"] = [this](const Json& c) {
        Json models = Json::array();
        for (const auto& model : m_models.availableModels()) {
            models.push_back(modelJson(model));
        }
        return success(c, "get_available_models", Json{{"models", models}});
    };
    m_handlers["set_thinking_level"] = [this](const Json& c) { return setThinkingLevel(c); };
    m_handlers["cycle_thinking_level"] = [this](const Json& c) {
        const auto level = m_runtime.session().cycleThinkingLevel(false);
        return success(c, "cycle_thinking_level",
                       level ? Json{{"level", m_messages.thinkingLevelName(*level)}} : Json(nullptr));
    };
    m_handlers["get_available_thinking_levels"] = [this](const Json& c) {
        Json levels = Json::array();
        for (const auto level : m_runtime.session().availableThinkingLevels()) {
            levels.push_back(m_messages.thinkingLevelName(level));
        }
        return success(c, "get_available_thinking_levels", Json{{"levels", levels}});
    };
    m_handlers["set_steering_mode"] = [this](const Json& c) { return setMode(c, "set_steering_mode", true); };
    m_handlers["set_follow_up_mode"] = [this](const Json& c) { return setMode(c, "set_follow_up_mode", false); };
}

void RpcCommandRouter::registerSessionCommands() {
    m_handlers["new_session"] = [this](const Json& c) {
        const std::string parent = text(c, "parentSession");
        detach();
        const auto outcome = m_runtime.newSession(parent.empty() ? std::nullopt : std::optional<std::string>(parent));
        return replaced(c, "new_session", outcome, Json{{"cancelled", false}});
    };
    m_handlers["switch_session"] = [this](const Json& c) {
        detach();
        const auto outcome = m_runtime.switchSession(text(c, "sessionPath"), std::nullopt);
        return replaced(c, "switch_session", outcome, Json{{"cancelled", false}});
    };
    m_handlers["fork"] = [this](const Json& c) { return fork(c, false); };
    m_handlers["clone"] = [this](const Json& c) { return fork(c, true); };
    m_handlers["get_fork_messages"] = [this](const Json& c) {
        return success(c, "get_fork_messages", Json{{"messages", m_data.forkable(m_runtime.session().forkableMessages())}});
    };
    m_handlers["get_entries"] = [this](const Json& c) { return getEntries(c); };
    m_handlers["get_tree"] = [this](const Json& c) {
        IAgentSession& session = m_runtime.session();
        return success(c, "get_tree",
                       Json{{"tree", m_data.tree(session.tree())}, {"leafId", m_data.optionalString(session.leafId())}});
    };
    m_handlers["set_session_name"] = [this](const Json& c) { return setSessionName(c); };
    m_handlers["export_html"] = [this](const Json& c) {
        return failure(c, "export_html", "export_html is not supported by the headless backbone");
    };
}

void RpcCommandRouter::registerStateCommands() {
    m_handlers["get_state"] = [this](const Json& c) { return getState(c); };
    m_handlers["get_session_stats"] = [this](const Json& c) {
        return success(c, "get_session_stats", m_data.stats(m_runtime.session().stats()));
    };
    m_handlers["get_last_assistant_text"] = [this](const Json& c) {
        return success(c, "get_last_assistant_text",
                       Json{{"text", m_data.optionalString(m_runtime.session().lastAssistantText())}});
    };
    m_handlers["get_messages"] = [this](const Json& c) {
        return success(c, "get_messages",
                       Json{{"messages", m_agentMessages.listToJson(m_runtime.session().messages())}});
    };
    m_handlers["get_commands"] = [this](const Json& c) {
        return success(c, "get_commands", Json{{"commands", m_data.slashCommands(m_runtime.session().slashCommands())}});
    };
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

std::optional<Json> RpcCommandRouter::prompt(const Json& command) {
    PromptOptions options;
    options.images = images(command);
    const std::string behavior = text(command, "streamingBehavior");
    options.streamingBehavior = behavior == "steer"      ? StreamingBehavior::Steer
                                : behavior == "followUp" ? StreamingBehavior::FollowUp
                                                         : StreamingBehavior::None;
    bool answered = false;
    options.onDisposition = [&](PromptDisposition disposition) {
        answered = true;
        m_output(success(command, "prompt",
                         Json{{"disposition", disposition == PromptDisposition::Started ? "started" : "queued"}}));
    };
    const auto outcome = m_runtime.session().prompt(text(command, "message"), options);
    if (!outcome && !answered) {
        return failure(command, "prompt", outcome.error().message);
    }
    return std::nullopt;
}

std::optional<Json> RpcCommandRouter::queueInput(const Json& command, const std::string& name, bool steering) {
    IAgentSession& session = m_runtime.session();
    const auto outcome = steering ? session.steer(text(command, "message"), images(command))
                                  : session.followUp(text(command, "message"), images(command));
    if (!outcome) {
        return failure(command, name, outcome.error().message);
    }
    return success(command, name, Json{{"disposition", "queued"}});
}

std::optional<Json> RpcCommandRouter::abort(const Json& command) {
    IAgentSession& session = m_runtime.session();
    session.abort();
    session.waitForIdle();
    return success(command, "abort");
}

std::optional<Json> RpcCommandRouter::getState(const Json& command) {
    IAgentSession& session = m_runtime.session();
    Json state = Json::object();
    const Model model = session.model();
    if (!model.id.empty()) {
        state["model"] = modelJson(model);
    }
    state["thinkingLevel"] = m_messages.thinkingLevelName(session.thinkingLevel());
    state["isStreaming"] = session.isStreaming();
    state["isCompacting"] = session.isCompacting();
    state["steeringMode"] = queueModeName(session.steeringMode());
    state["followUpMode"] = queueModeName(session.followUpMode());
    if (const auto file = session.sessionFile()) {
        state["sessionFile"] = *file;
    }
    state["sessionId"] = session.sessionId();
    if (const auto name = session.sessionName()) {
        state["sessionName"] = *name;
    }
    state["autoCompactionEnabled"] = session.autoCompactionEnabled();
    state["messageCount"] = session.messages().size();
    state["pendingMessageCount"] = session.pendingMessageCount();
    return success(command, "get_state", state);
}

std::optional<Json> RpcCommandRouter::setModel(const Json& command) {
    const std::string provider = text(command, "provider");
    const std::string id = text(command, "modelId");
    for (const auto& model : m_models.availableModels()) {
        if (model.provider == provider && model.id == id) {
            if (const auto outcome = m_runtime.session().setModel(model, false); !outcome) {
                return failure(command, "set_model", outcome.error().message);
            }
            return success(command, "set_model", modelJson(model));
        }
    }
    return failure(command, "set_model", "Model not found: " + provider + "/" + id);
}

std::optional<Json> RpcCommandRouter::setThinkingLevel(const Json& command) {
    const auto level = m_messages.parseThinkingLevel(text(command, "level"));
    if (!level) {
        return failure(command, "set_thinking_level", "Unknown thinking level: " + text(command, "level"));
    }
    m_runtime.session().setThinkingLevel(*level, false);
    return success(command, "set_thinking_level");
}

std::optional<Json> RpcCommandRouter::setMode(const Json& command, const std::string& name, bool steering) {
    const QueueMode mode = parseQueueMode(text(command, "mode"));
    if (steering) {
        m_runtime.session().setSteeringMode(mode);
    } else {
        m_runtime.session().setFollowUpMode(mode);
    }
    return success(command, name);
}

std::optional<Json> RpcCommandRouter::compact(const Json& command) {
    const std::string instructions = text(command, "customInstructions");
    const auto result = m_runtime.session().compact(instructions.empty() ? std::nullopt
                                                                          : std::optional<std::string>(instructions));
    if (!result) {
        return failure(command, "compact", result.error().message);
    }
    return success(command, "compact", m_data.compaction(*result));
}

std::optional<Json> RpcCommandRouter::bash(const Json& command) {
    std::optional<std::string> id;
    if (command.contains("id") && command["id"].is_string()) {
        id = command["id"].get<std::string>();
    }
    const auto result = m_runtime.session().executeBash(text(command, "command"), nullptr,
                                                        flag(command, "excludeFromContext"), id);
    if (!result) {
        return failure(command, "bash", result.error().message);
    }
    return success(command, "bash", m_data.bash(*result));
}

/** The old session is gone after a replacement; follow whichever session is current now. */
Json RpcCommandRouter::replaced(const Json& command, const std::string& name, const Result<void>& outcome, Json data) {
    attach();
    if (!outcome) {
        return failure(command, name, outcome.error().message);
    }
    return success(command, name, data);
}

std::optional<Json> RpcCommandRouter::fork(const Json& command, bool clone) {
    const char* name = clone ? "clone" : "fork";
    std::string entryId = text(command, "entryId");
    if (clone) {
        const auto leaf = m_runtime.session().leafId();
        if (!leaf) {
            return failure(command, name, "Cannot clone session: no current entry selected");
        }
        entryId = *leaf;
    }
    detach();
    const auto result = m_runtime.fork(entryId, clone ? ForkPosition::At : ForkPosition::Before);
    attach();
    if (!result) {
        return failure(command, name, result.error().message);
    }
    Json data = Json{{"cancelled", false}};
    if (!clone && result->selectedText) {
        data["text"] = *result->selectedText;
    }
    return success(command, name, data);
}

std::optional<Json> RpcCommandRouter::getEntries(const Json& command) {
    IAgentSession& session = m_runtime.session();
    std::vector<SessionEntry> entries = session.entries();
    const std::string since = text(command, "since");
    if (command.contains("since")) {
        const auto at = std::ranges::find_if(entries, [&](const SessionEntry& entry) { return entry.id == since; });
        if (at == entries.end()) {
            return failure(command, "get_entries", "Entry not found: " + since);
        }
        entries.erase(entries.begin(), at + 1);
    }
    Json list = Json::array();
    for (const auto& entry : entries) {
        list.push_back(entry.body);
    }
    return success(command, "get_entries", Json{{"entries", list}, {"leafId", m_data.optionalString(session.leafId())}});
}

std::optional<Json> RpcCommandRouter::setSessionName(const Json& command) {
    std::string name = text(command, "name");
    const auto first = name.find_first_not_of(" \t\r\n");
    name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    if (name.empty()) {
        return failure(command, "set_session_name", "Session name cannot be empty");
    }
    m_runtime.session().setSessionName(name);
    return success(command, "set_session_name");
}
