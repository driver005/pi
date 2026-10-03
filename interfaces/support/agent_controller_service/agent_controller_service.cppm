export module pi.support.agent_controller_service;

import std;
export import pi.chord.i_remote_service;
export import pi.platform.i_executor;
export import pi.platform.i_id_generator;
export import pi.session.i_agent_session;
export import pi.types.prompt_operation;
export import pi.types.queued_entry;

/**
 * The `pi.agent-controller` service: a presentation-safe command facade over one agent session
 * (prompt, steer, follow-up, withdraw queued input, abort, compact, wait for an answer). Runs the
 * model work on the executor so a call returns as soon as the work was accepted or refused.
 * Counterpart of agent-controller-provider.ts, over an AgentSession instead of the durable
 * conversation. Responses use the TS shapes: `{accepted, operationId | entryId, error}`.
 */
export class AgentControllerService : public IRemoteService {
public:
    AgentControllerService(IAgentSession& session, IExecutor& executor, IIdGenerator& ids);
    ~AgentControllerService() override;

    AgentControllerService(const AgentControllerService&) = delete;
    AgentControllerService& operator=(const AgentControllerService&) = delete;

    std::map<std::string, Method> methods() override;
    std::map<std::string, IReplicatedState*> states() override;

private:
    Result<std::optional<Json>> prompt(const std::vector<Json>& args);
    Result<std::optional<Json>> queueInput(const std::vector<Json>& args, bool steering);
    Result<std::optional<Json>> cancelQueued(const std::vector<Json>& args);
    Result<std::optional<Json>> abort();
    Result<std::optional<Json>> compact(const std::vector<Json>& args);
    Result<std::optional<Json>> waitForPrompt(const std::vector<Json>& args, const ServiceContext& context);
    Result<std::pair<std::string, std::vector<ImageContent>>> parseRequest(const std::vector<Json>& args) const;
    Json startRun(const std::string& text, const std::vector<ImageContent>& images, const std::string& key);
    void runPrompt(const std::string& text, const std::vector<ImageContent>& images,
                   const std::shared_ptr<PromptOperation>& operation, const std::string& id, const std::string& key,
                   const std::shared_ptr<std::promise<Json>>& answer);
    void runCompaction(const std::optional<std::string>& instructions, const std::shared_ptr<PromptOperation>& operation);
    void settle(const std::shared_ptr<PromptOperation>& operation, const std::string& status,
                const std::optional<std::string>& text, const std::optional<std::string>& reason);
    std::shared_ptr<PromptOperation> registerOperation(const std::string& id);
    void forget(const std::string& id);
    void markIdle();
    void finishTask();
    Json accepted(const std::string& key, const std::string& id) const;
    Json rejected(const std::string& key, const Error& error) const;
    std::string errorCode(const Error& error) const;

    IAgentSession& m_session;
    IExecutor& m_executor;
    IIdGenerator& m_ids;

    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_active = false;
    int m_running = 0;
    std::uint64_t m_abortGeneration = 0;
    std::map<std::string, std::shared_ptr<PromptOperation>> m_operations;
    std::deque<std::string> m_operationOrder;

    std::mutex m_queueMutex;
    std::vector<QueuedEntry> m_queued;
};

AgentControllerService::AgentControllerService(IAgentSession& session, IExecutor& executor, IIdGenerator& ids)
    : m_session(session), m_executor(executor), m_ids(ids) {}

AgentControllerService::~AgentControllerService() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [&] { return m_running == 0; });
}

std::map<std::string, IReplicatedState*> AgentControllerService::states() {
    return {};
}

std::map<std::string, IRemoteService::Method> AgentControllerService::methods() {
    std::map<std::string, Method> methods;
    methods["prompt"] = [this](const std::vector<Json>& args, const ServiceContext&) { return prompt(args); };
    methods["steer"] = [this](const std::vector<Json>& args, const ServiceContext&) { return queueInput(args, true); };
    methods["followUp"] = [this](const std::vector<Json>& args, const ServiceContext&) {
        return queueInput(args, false);
    };
    methods["cancelQueued"] = [this](const std::vector<Json>& args, const ServiceContext&) {
        return cancelQueued(args);
    };
    methods["abort"] = [this](const std::vector<Json>&, const ServiceContext&) { return abort(); };
    methods["compact"] = [this](const std::vector<Json>& args, const ServiceContext&) { return compact(args); };
    methods["waitForPrompt"] = [this](const std::vector<Json>& args, const ServiceContext& context) {
        return waitForPrompt(args, context);
    };
    return methods;
}

Json AgentControllerService::accepted(const std::string& key, const std::string& id) const {
    return Json{{"accepted", true}, {key, id}, {"error", nullptr}};
}

Json AgentControllerService::rejected(const std::string& key, const Error& error) const {
    return Json{{"accepted", false},
                {key, nullptr},
                {"error", Json{{"code", errorCode(error)}, {"message", error.message}}}};
}

std::string AgentControllerService::errorCode(const Error& error) const {
    return error.code == "agent_busy" ? "busy" : error.code;
}

Result<std::pair<std::string, std::vector<ImageContent>>> AgentControllerService::parseRequest(
    const std::vector<Json>& args) const {
    const Error invalid{"invalid_request", "Invalid agent prompt request"};
    if (args.size() != 1 || !args[0].is_object() || !args[0].contains("message") || !args[0]["message"].is_string()) {
        return std::unexpected(invalid);
    }
    std::vector<ImageContent> images;
    const Json imagesJson = args[0].value("images", Json());
    if (imagesJson.is_array()) {
        for (const Json& image : imagesJson) {
            if (!image.is_object() || !image.value("data", Json()).is_string() ||
                !image.value("mimeType", Json()).is_string()) {
                return std::unexpected(invalid);
            }
            images.push_back(ImageContent{image["data"].get<std::string>(), image["mimeType"].get<std::string>()});
        }
    } else if (!imagesJson.is_null()) {
        return std::unexpected(invalid);
    }
    return std::make_pair(args[0]["message"].get<std::string>(), std::move(images));
}

std::shared_ptr<PromptOperation> AgentControllerService::registerOperation(const std::string& id) {
    auto operation = std::make_shared<PromptOperation>();
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_operations[id] = operation;
    m_operationOrder.push_back(id);
    while (m_operationOrder.size() > 64) {
        m_operations.erase(m_operationOrder.front());
        m_operationOrder.pop_front();
    }
    return operation;
}

void AgentControllerService::forget(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_operations.erase(id);
    std::erase(m_operationOrder, id);
}

void AgentControllerService::settle(const std::shared_ptr<PromptOperation>& operation, const std::string& status,
                                    const std::optional<std::string>& text, const std::optional<std::string>& reason) {
    {
        const std::lock_guard<std::mutex> lock(operation->mutex);
        operation->done = true;
        operation->status = status;
        operation->text = text;
        operation->reason = reason;
    }
    operation->changed.notify_all();
}

void AgentControllerService::markIdle() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_active = false;
}

void AgentControllerService::finishTask() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        --m_running;
    }
    m_changed.notify_all();
}

Json AgentControllerService::startRun(const std::string& text, const std::vector<ImageContent>& images,
                                      const std::string& key) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active) {
            return rejected(key, Error{"busy", "A run is already active"});
        }
        m_active = true;
        ++m_running;
    }
    const std::string id = m_ids.next();
    const auto operation = registerOperation(id);
    const auto answer = std::make_shared<std::promise<Json>>();
    std::future<Json> response = answer->get_future();
    m_executor.submit([this, text, images, operation, id, key, answer] { runPrompt(text, images, operation, id, key, answer); });
    return response.get();
}

void AgentControllerService::runPrompt(const std::string& text, const std::vector<ImageContent>& images,
                                       const std::shared_ptr<PromptOperation>& operation, const std::string& id,
                                       const std::string& key, const std::shared_ptr<std::promise<Json>>& answer) {
    std::uint64_t generation = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        generation = m_abortGeneration;
    }
    PromptOptions options;
    options.images = images;
    bool answered = false;
    options.onDisposition = [&](PromptDisposition) {
        answered = true;
        answer->set_value(accepted(key, id));
    };
    const auto outcome = m_session.prompt(text, options);
    if (!answered) {
        answer->set_value(outcome ? accepted(key, id) : rejected(key, outcome.error()));
        if (!outcome) {
            forget(id);
            markIdle();
            settle(operation, "unanswered", std::nullopt, outcome.error().message);
            finishTask();
            return;
        }
    }
    bool aborted = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        aborted = m_abortGeneration != generation;
    }
    const auto answerText = m_session.lastAssistantText();
    markIdle();
    if (outcome && answerText && !aborted) {
        settle(operation, "done", answerText, std::nullopt);
    } else {
        settle(operation, "unanswered", std::nullopt,
               aborted ? "The run was aborted" : outcome ? "The prompt produced no answer" : outcome.error().message);
    }
    finishTask();
}

Result<std::optional<Json>> AgentControllerService::prompt(const std::vector<Json>& args) {
    auto request = parseRequest(args);
    if (!request) {
        return std::unexpected(request.error());
    }
    return std::optional<Json>(startRun(request->first, request->second, "operationId"));
}

Result<std::optional<Json>> AgentControllerService::queueInput(const std::vector<Json>& args, bool steering) {
    auto request = parseRequest(args);
    if (!request) {
        return std::unexpected(request.error());
    }
    const auto& [text, images] = *request;
    bool idle = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        idle = !m_active && m_session.isIdle();
    }
    if (idle) {
        return std::optional<Json>(startRun(text, images, "entryId"));
    }
    const auto outcome = steering ? m_session.steer(text, images) : m_session.followUp(text, images);
    if (!outcome) {
        return std::optional<Json>(rejected("entryId", outcome.error()));
    }
    const std::string id = m_ids.next();
    const std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queued.push_back(QueuedEntry{id, steering, text, images});
    return std::optional<Json>(accepted("entryId", id));
}

Result<std::optional<Json>> AgentControllerService::cancelQueued(const std::vector<Json>& args) {
    if (args.size() != 1 || !args[0].is_string()) {
        return std::unexpected(Error{"invalid_request", "Invalid cancelQueued request"});
    }
    const std::string target = args[0].get<std::string>();
    const std::lock_guard<std::mutex> lock(m_queueMutex);
    const bool known = std::ranges::any_of(m_queued, [&](const QueuedEntry& entry) { return entry.id == target; });
    if (!known) {
        return std::optional<Json>(Json{{"outcome", "not_found"}});
    }
    QueuedInput pending = m_session.clearQueue();
    std::string outcome = "already_consumed";
    std::vector<QueuedEntry> kept;
    for (QueuedEntry& entry : m_queued) {
        std::vector<std::string>& left = entry.steering ? pending.steering : pending.followUp;
        const auto found = std::ranges::find(left, entry.text);
        if (found == left.end()) {
            continue;
        }
        left.erase(found);
        if (entry.id == target) {
            outcome = "cancelled";
            continue;
        }
        kept.push_back(entry);
    }
    for (const QueuedEntry& entry : kept) {
        entry.steering ? m_session.steer(entry.text, entry.images) : m_session.followUp(entry.text, entry.images);
    }
    for (const std::string& text : pending.steering) {
        m_session.steer(text, {});
    }
    for (const std::string& text : pending.followUp) {
        m_session.followUp(text, {});
    }
    m_queued = std::move(kept);
    return std::optional<Json>(Json{{"outcome", outcome}});
}

Result<std::optional<Json>> AgentControllerService::abort() {
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queued.clear();
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_abortGeneration;
    }
    m_session.clearQueue();
    m_session.abort();
    m_session.waitForIdle();
    return std::optional<Json>();
}

void AgentControllerService::runCompaction(const std::optional<std::string>& instructions,
                                           const std::shared_ptr<PromptOperation>& operation) {
    const auto outcome = m_session.compact(instructions);
    markIdle();
    if (outcome) {
        settle(operation, "done", std::string(), std::nullopt);
    } else {
        settle(operation, "unanswered", std::nullopt, outcome.error().message);
    }
    finishTask();
}

Result<std::optional<Json>> AgentControllerService::compact(const std::vector<Json>& args) {
    if (args.size() != 1 || !args[0].is_object()) {
        return std::unexpected(Error{"invalid_request", "Invalid compaction request"});
    }
    std::optional<std::string> instructions;
    const Json custom = args[0].value("customInstructions", Json());
    if (custom.is_string()) {
        instructions = custom.get<std::string>();
    } else if (!custom.is_null()) {
        return std::unexpected(Error{"invalid_request", "Invalid compaction request"});
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active || !m_session.isIdle()) {
            return std::optional<Json>(rejected("operationId", Error{"busy", "A run is already active"}));
        }
        m_active = true;
        ++m_running;
    }
    const std::string id = m_ids.next();
    const auto operation = registerOperation(id);
    m_executor.submit([this, instructions, operation] { runCompaction(instructions, operation); });
    return std::optional<Json>(accepted("operationId", id));
}

Result<std::optional<Json>> AgentControllerService::waitForPrompt(const std::vector<Json>& args,
                                                                  const ServiceContext& context) {
    if (args.size() != 1 || !args[0].is_string()) {
        return std::unexpected(Error{"invalid_request", "Invalid waitForPrompt request"});
    }
    std::shared_ptr<PromptOperation> operation;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_operations.find(args[0].get<std::string>());
        if (found != m_operations.end()) {
            operation = found->second;
        }
    }
    if (!operation) {
        return std::unexpected(Error{"operation_not_found", "Unknown or expired operation"});
    }
    std::unique_lock<std::mutex> lock(operation->mutex);
    while (!operation->done) {
        if (context.signal && context.signal->aborted()) {
            return std::unexpected(Error{"cancelled", "RPC request cancelled"});
        }
        operation->changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    return std::optional<Json>(Json{{"status", operation->status},
                                    {"text", operation->text ? Json(*operation->text) : Json(nullptr)},
                                    {"reason", operation->reason ? Json(*operation->reason) : Json(nullptr)}});
}
