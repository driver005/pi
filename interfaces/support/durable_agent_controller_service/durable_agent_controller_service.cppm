export module pi.support.durable_agent_controller_service;

import std;
export import pi.chord.i_remote_service;
export import pi.plugin.i_hook_bus;
export import pi.plugin.i_plugin_commands;
export import pi.support.durable_session_bridge;
export import pi.support.harness;
export import pi.support.plugin_session_events;

/**
 * The `pi.agent-controller` service over a durable conversation: prompt, steer, follow-up, withdraw queued input, abort,
 * compact and wait for an answer. Every operation is one durable submission or task, so `operationId` and `entryId` are
 * submission ids (a compaction's `operationId` is its task id) and survive a restart. Responses use the shapes of
 * agent-controller.ts: `{accepted, operationId | entryId, error}`. Port of agent-controller-provider.ts. With a hook bus,
 * plugins see the input first (`input`: handled input is rejected with code `input_handled`, transformed input replaces
 * the message and images) and a started prompt (`before_agent_start`: the messages plugins add travel as further text
 * blocks of the prompt; a replacement system prompt is not supported, the durable prompt is assembled by the registry).
 * With plugin commands, a message `/name args` naming one runs it instead of reaching the model (before the input event,
 * as in AgentSession), with the session calls of the command bridged to this conversation (`DurableSessionBridge`); the
 * request is answered `accepted: false` with code `command_handled`, or `command_failed` carrying the command's error, and
 * `abort` cancels a command that is still running.
 */
export class DurableAgentControllerService : public IRemoteService {
public:
    DurableAgentControllerService(Harness& harness, std::shared_ptr<Conversation> conversation, std::shared_ptr<IHookBus> hooks = {},
                                  IPluginCommands* commands = nullptr, std::string cwd = {})
        : m_harness(harness),
          m_conversation(std::move(conversation)),
          m_hooks(std::move(hooks)),
          m_commands(commands),
          m_bridge(m_conversation, std::move(cwd)) {
        if (m_hooks) {
            m_events = std::make_unique<PluginSessionEvents>(*m_hooks);
        }
    }

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["prompt"] = [this](const std::vector<Json>& args, const ServiceContext& context) { return submit(args, "reject", "operationId", context); };
        methods["steer"] = [this](const std::vector<Json>& args, const ServiceContext& context) { return submit(args, "steer", "entryId", context); };
        methods["followUp"] = [this](const std::vector<Json>& args, const ServiceContext& context) { return submit(args, "followUp", "entryId", context); };
        methods["cancelQueued"] = [this](const std::vector<Json>& args, const ServiceContext&) { return cancelQueued(args); };
        methods["abort"] = [this](const std::vector<Json>&, const ServiceContext&) { return abort(); };
        methods["compact"] = [this](const std::vector<Json>& args, const ServiceContext&) { return compact(args); };
        methods["waitForPrompt"] = [this](const std::vector<Json>& args, const ServiceContext& context) { return waitForPrompt(args, context); };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {};
    }

private:
    Result<std::optional<Json>> submit(const std::vector<Json>& args, const std::string& whenBusy, const std::string& key, const ServiceContext& context) {
        auto content = parseRequest(args);
        if (!content) {
            return std::unexpected(content.error());
        }
        if (auto handled = runCommand(*content, key, context)) {
            return std::optional<Json>(*handled);
        }
        if (m_events) {
            if (auto intercepted = intercept(*content, whenBusy); !intercepted) {
                return std::optional<Json>(Json{{"accepted", false},
                                                {key, nullptr},
                                                {"error", Json{{"code", "input_handled"}, {"message", "Input was handled by a plugin"}}}});
            }
        }
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = std::move(*content);
        draft.whenBusy = whenBusy;
        auto submission = m_conversation->submit(draft);
        if (!submission) {
            return std::optional<Json>(rejected(key, submission.error()));
        }
        return std::optional<Json>(accepted(key, std::to_string((*submission)->id())));
    }

    /** The answer of a plugin command the text names, run on this conversation; nothing when it names none. */
    std::optional<Json> runCommand(const Json& content, const std::string& key, const ServiceContext& context) {
        if (m_commands == nullptr || !content.is_string() || !content.get<std::string>().starts_with("/")) {
            return std::nullopt;
        }
        const std::string text = content.get<std::string>();
        const std::size_t space = text.find(' ');
        const std::string name = text.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        const std::string args = space == std::string::npos ? std::string() : text.substr(space + 1);
        const auto signal = std::make_shared<AbortSignal>();
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandAbort = signal;
        }
        const std::uint64_t listener = context.signal ? context.signal->onAbort([signal] { signal->abort(); }) : 0;
        const auto outcome = m_commands->execute(name, args, signal, &m_bridge);
        if (context.signal) {
            context.signal->removeListener(listener);
        }
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commandAbort = nullptr;
        }
        if (!outcome) {
            return std::nullopt;
        }
        if (!*outcome) {
            return Json{{"accepted", false}, {key, nullptr}, {"error", Json{{"code", "command_failed"}, {"message", outcome->error().message}}}};
        }
        return Json{{"accepted", false}, {key, nullptr}, {"error", Json{{"code", "command_handled"}, {"message", "Handled by the plugin command /" + name}}}};
    }

    /** Runs the input and prompt plugin events over `content` in place; nullopt when a plugin handled the input. */
    std::optional<bool> intercept(Json& content, const std::string& whenBusy) {
        std::string text;
        std::vector<ImageContent> images;
        if (content.is_string()) {
            text = content.get<std::string>();
        } else {
            for (const Json& block : content) {
                if (block.value("type", std::string()) == "text" && text.empty()) {
                    text = block.value("text", std::string());
                } else if (block.value("type", std::string()) == "image") {
                    images.push_back(ImageContent{block.value("data", std::string()), block.value("mimeType", std::string())});
                }
            }
        }
        const InputOutcome input = m_events->input(text, images, "rpc", whenBusy == "reject" ? "" : whenBusy);
        if (input.handled) {
            return std::nullopt;
        }
        AgentStartOutcome start;
        if (whenBusy == "reject") {
            start = m_events->beforeAgentStart(input.text, input.images, "");
        }
        Json blocks = Json::array({Json{{"type", "text"}, {"text", input.text}}});
        for (const ImageContent& image : input.images) {
            blocks.push_back(Json{{"type", "image"}, {"data", image.data}, {"mimeType", image.mimeType}});
        }
        for (const Json& added : start.messages) {
            const Json& body = added["content"];
            if (body.is_string()) {
                blocks.push_back(Json{{"type", "text"}, {"text", body}});
            } else if (body.is_array()) {
                for (const Json& block : body) {
                    if (block.is_object() && (block.value("type", std::string()) == "text" || block.value("type", std::string()) == "image")) {
                        blocks.push_back(block);
                    }
                }
            }
        }
        content = blocks.size() == 1 ? Json(input.text) : blocks;
        return true;
    }

    Result<std::optional<Json>> cancelQueued(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid cancelQueued request"});
        }
        const std::optional<std::int64_t> id = parseId(args[0].get<std::string>());
        if (!id) {
            return std::optional<Json>(Json{{"outcome", "not_found"}});
        }
        auto result = m_harness.abortSubmission(*id, m_conversation->id());
        if (!result) {
            return std::unexpected(result.error());
        }
        const std::string outcome = *result == "aborted" ? "cancelled" : *result == "not_found" ? "not_found" : "already_consumed";
        return std::optional<Json>(Json{{"outcome", outcome}});
    }

    Result<std::optional<Json>> abort() {
        std::shared_ptr<AbortSignal> command;
        {
            const std::lock_guard<std::mutex> lock(m_commandMutex);
            command = m_commandAbort;
        }
        if (command) {
            command->abort();
        }
        if (auto aborted = m_conversation->abort(); !aborted) {
            return std::unexpected(aborted.error());
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> compact(const std::vector<Json>& args) {
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
        auto task = m_conversation->compact(instructions);
        if (!task) {
            return std::optional<Json>(rejected("operationId", task.error()));
        }
        return std::optional<Json>(accepted("operationId", std::to_string(*task)));
    }

    Result<std::optional<Json>> waitForPrompt(const std::vector<Json>& args, const ServiceContext& context) {
        if (args.size() != 1 || !args[0].is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid waitForPrompt request"});
        }
        const std::string operationId = args[0].get<std::string>();
        const std::optional<std::int64_t> id = parseId(operationId);
        std::shared_ptr<ISubmission> submission;
        if (id) {
            auto found = m_harness.submission(*id);
            if (!found) {
                return std::unexpected(found.error());
            }
            submission = *found;
        }
        if (!submission) {
            return std::unexpected(Error{"operation_not_found", "Unknown prompt: " + operationId});
        }
        auto settled = submission->wait(context.signal.get());
        if (!settled) {
            return std::unexpected(settled.error());
        }
        if (settled->value("status", std::string()) == "unanswered") {
            return std::optional<Json>(Json{{"status", "unanswered"}, {"text", nullptr}, {"reason", settled->value("reason", std::string())}});
        }
        auto text = answerText(*settled);
        if (!text) {
            return std::unexpected(text.error());
        }
        return std::optional<Json>(Json{{"status", "done"}, {"text", *text}, {"reason", nullptr}});
    }

    /** The text of the answering assistant entry of a settled input; empty for a settled write. */
    Result<std::string> answerText(const Json& settled) {
        if (settled.value("type", std::string()) != "input" || !settled.contains("answer")) {
            return std::string();
        }
        const std::int64_t answer = settled.at("answer").get<std::int64_t>();
        auto page = m_conversation->entries(answer, answer, 1);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (page->items.empty() || !page->items[0].contains("model") || page->items[0].at("model").empty()) {
            return std::string();
        }
        const Json& message = page->items[0].at("model")[0];
        std::string text;
        if (message.value("role", std::string()) == "assistant" && message.contains("content")) {
            for (const Json& block : message.at("content")) {
                if (block.value("type", std::string()) == "text") {
                    text += block.value("text", std::string());
                }
            }
        }
        return text;
    }

    /** `{message, images}` as the user content of a submission: a string, or text then image blocks. */
    Result<Json> parseRequest(const std::vector<Json>& args) const {
        const Error invalid{"invalid_request", "Invalid agent prompt request"};
        if (args.size() != 1 || !args[0].is_object() || !args[0].value("message", Json()).is_string()) {
            return std::unexpected(invalid);
        }
        const std::string message = args[0].at("message").get<std::string>();
        const Json images = args[0].value("images", Json());
        if (images.is_null() || (images.is_array() && images.empty())) {
            return Json(message);
        }
        if (!images.is_array()) {
            return std::unexpected(invalid);
        }
        Json content = Json::array({Json{{"type", "text"}, {"text", message}}});
        for (const Json& image : images) {
            if (!image.is_object() || !image.value("data", Json()).is_string() || !image.value("mimeType", Json()).is_string()) {
                return std::unexpected(invalid);
            }
            content.push_back(Json{{"type", "image"}, {"data", image.at("data")}, {"mimeType", image.at("mimeType")}});
        }
        return content;
    }

    /** A positive decimal submission id; nothing for other text. */
    std::optional<std::int64_t> parseId(const std::string& text) const {
        if (text.empty() || text.size() > 15 || text[0] == '0' || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        return std::strtoll(text.c_str(), nullptr, 10);
    }

    Json accepted(const std::string& key, const std::string& id) const {
        return Json{{"accepted", true}, {key, id}, {"error", nullptr}};
    }

    Json rejected(const std::string& key, const Error& error) const {
        return Json{{"accepted", false},
                    {key, nullptr},
                    {"error", Json{{"code", error.code == "conversation_busy" ? "busy" : "operation_failed"}, {"message", error.message}}}};
    }

    Harness& m_harness;
    std::shared_ptr<Conversation> m_conversation;
    std::shared_ptr<IHookBus> m_hooks;
    std::unique_ptr<PluginSessionEvents> m_events;
    IPluginCommands* m_commands;
    DurableSessionBridge m_bridge;
    std::mutex m_commandMutex;
    std::shared_ptr<AbortSignal> m_commandAbort;
};
