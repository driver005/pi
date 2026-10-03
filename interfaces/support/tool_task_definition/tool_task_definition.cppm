module;

#include <cstdint>

export module pi.support.tool_task_definition;

import std;
export import pi.durable.task_definition;
export import pi.support.builtin_documents;
export import pi.support.entry_kinds;
export import pi.support.live_editor;
export import pi.support.output_bounder;
export import pi.support.schema_validator;
export import pi.support.tool_call_api;
export import pi.support.tool_results;
export import pi.types.bounded_content;
export import pi.types.output_limits;
export import pi.types.tool_registration;

/**
 * The built-in tool task (`pi.tool`): resolves the called tool among its phase agent's tools, validates, runs
 * `beforeTool`, records intent, executes, runs `afterTool`, and appends the result, all in one `call` handler so nothing
 * separates resolution from settlement. `execute` is reached only by recovery and applies the replay rule. Port of
 * packages/durable/src/harness/tool.ts.
 *
 * Input `{assistant: entryId, callId}`. Checkpoints `{phase: "call"}` and `{phase: "execute", arguments, replay}`.
 * Result `{entryId, control?}`.
 */
export class ToolTaskDefinition {
public:
    std::string kind() const {
        return "pi.tool";
    }

    std::shared_ptr<TaskDefinition> build() const {
        auto definition = std::make_shared<TaskDefinition>();
        definition->name = kind();
        definition->version = 1;
        definition->initial = [](const Json&) { return Json::object({{"phase", "call"}}); };
        definition->phases["call"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.call(task, runtime); };
        definition->phases["execute"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.execute(task, runtime); };
        definition->abort = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.abort(task, runtime); };
        return definition;
    }

private:
    // ─── Phases ─────────────────────────────────────────────────────────────

    Result<void> call(const Json& task, ITaskRuntime& runtime) const {
        auto call = readCall(runtime, task.at("input"));
        if (!call) {
            return std::unexpected(call.error());
        }
        auto agent = runtime.agent();
        if (!agent) {
            return std::unexpected(agent.error());
        }
        const ToolRegistration* tool = find(**agent, call->at("name").get<std::string>());
        if (tool == nullptr) {
            return settle(runtime, *call, "completed", "", [&](const Json*) {
                return m_results.harnessError("tool_unavailable", "Tool " + call->at("name").get<std::string>() + " is not available");
            });
        }
        Json args = call->at("arguments");
        auto checked = prepareAndValidate(*tool, args);
        if (!checked) {
            return settleInvalid(runtime, *call, checked.error().message);
        }
        args = *checked;
        std::optional<std::string> block;
        auto hooked = runtime.eachHook("beforeTool", [&](const HookHandler& hook) -> Result<void> {
            if (block) {
                return {};
            }
            Json payload = *call;
            payload["arguments"] = args;
            auto decision = hook(payload, runtime);
            if (!decision) {
                if (runtime.signal().aborted()) {
                    return std::unexpected(decision.error());
                }
                block = decision.error().message;
            } else if (*decision && (*decision)->contains("block")) {
                block = (*decision)->at("block").get<std::string>();
            } else if (*decision && (*decision)->contains("arguments")) {
                args = (*decision)->at("arguments");
            }
            return {};
        });
        if (!hooked) {
            return hooked;
        }
        if (block) {
            return settle(runtime, *call, "completed", "", [&](const Json*) {
                return m_results.harnessError("blocked", "Tool call blocked: " + *block);
            });
        }
        auto validated = m_validator.validateArguments(tool->parameters, args);
        if (!validated) {
            return settleInvalid(runtime, *call, validated.error().message);
        }
        const Json finalArgs = *validated;
        auto intent = runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            DocAddressArgs address;
            address.owner = runtime.conversationId();
            auto live = tx.doc(m_documents.live(), address);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (Json* slot = m_live.toolSlot(**live, runtime.taskId())) {
                (*slot)["status"] = "running";
            }
            return std::optional<Json>(Json::object({{"status", "running"},
                                                     {"checkpoint", Json::object({{"phase", "execute"}, {"arguments", finalArgs}, {"replay", tool->replay}})}}));
        });
        if (!intent) {
            return intent;
        }
        return run(runtime, *call, *tool, finalArgs);
    }

    /** Recovery after intent: rerun only when the stored and the current policy both say `safe`. */
    Result<void> execute(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        auto call = readCall(runtime, task.at("input"));
        if (!call) {
            return std::unexpected(call.error());
        }
        auto agent = runtime.agent();
        if (!agent) {
            return std::unexpected(agent.error());
        }
        const ToolRegistration* tool = find(**agent, call->at("name").get<std::string>());
        if (checkpoint.at("replay") == "safe" && tool != nullptr && tool->replay == "safe") {
            // The rerun reports from scratch; clear what the interrupted attempt published.
            auto cleared = runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
                DocAddressArgs address;
                address.owner = runtime.conversationId();
                auto live = tx.doc(m_documents.live(), address);
                if (!live) {
                    return std::unexpected(live.error());
                }
                if (Json* slot = m_live.toolSlot(**live, runtime.taskId())) {
                    m_live.clearProgress(*slot);
                }
                return std::optional<Json>();
            });
            if (!cleared) {
                return cleared;
            }
            return run(runtime, *call, *tool, checkpoint.at("arguments"));
        }
        const std::string message = "Tool " + call->at("name").get<std::string>() + " was interrupted and may have partially run";
        // `failed` records cancellation intent, so the call's owned conversations, left unsupervised, are aborted.
        return settle(runtime, *call, "failed", message, [&](const Json* slot) { return fromSlot(slot, "interrupted", message); });
    }

    Result<void> abort(const Json& task, ITaskRuntime& runtime) const {
        auto call = readCall(runtime, task.at("input"));
        if (!call) {
            return std::unexpected(call.error());
        }
        const std::string message = "Tool " + call->at("name").get<std::string>() + " was aborted";
        return settle(runtime, *call, "aborted", "", [&](const Json* slot) { return fromSlot(slot, "aborted", message); });
    }

    // ─── Execution ──────────────────────────────────────────────────────────

    /** The tool call `callId` of the assistant entry. */
    Result<Json> readCall(ITaskRuntime& runtime, const Json& input) const {
        auto entry = runtime.entry(input.at("assistant").get<std::int64_t>(), m_kinds.assistant());
        if (!entry) {
            return std::unexpected(entry.error());
        }
        const std::string callId = input.at("callId").get<std::string>();
        if (*entry && (*entry)->contains("model") && !(*entry)->at("model").empty()) {
            const Json& message = (*entry)->at("model")[0];
            if (message.value("role", std::string()) == "assistant") {
                for (const Json& block : message.at("content")) {
                    if (block.value("type", std::string()) == "toolCall" && block.value("id", std::string()) == callId) {
                        return block;
                    }
                }
            }
        }
        return std::unexpected(Error{"durable_error", "Entry " + std::to_string(input.at("assistant").get<std::int64_t>()) +
                                                          " has no tool call " + callId});
    }

    const ToolRegistration* find(const IConversationAgent& agent, const std::string& name) const {
        for (const ToolRegistration& tool : agent.snapshot()->tools) {
            if (tool.name == name) {
                return &tool;
            }
        }
        return nullptr;
    }

    /** The call's arguments as repaired by the tool, validated and coerced against its schema. */
    Result<Json> prepareAndValidate(const ToolRegistration& tool, const Json& args) const {
        Json prepared = args;
        if (tool.prepareArguments) {
            auto repaired = tool.prepareArguments(args);
            if (!repaired) {
                return std::unexpected(repaired.error());
            }
            prepared = *repaired;
        }
        return m_validator.validateArguments(tool.parameters, prepared);
    }

    Result<void> settleInvalid(ITaskRuntime& runtime, const Json& call, const std::string& message) const {
        return settle(runtime, call, "completed", "", [&](const Json*) { return m_results.harnessError("invalid_arguments", message); });
    }

    /** Executes with the resolved implementation, then settles its result. */
    Result<void> run(ITaskRuntime& runtime, const Json& call, const ToolRegistration& tool, const Json& args) const {
        const OutputLimits limits = tool.outputLimits.value_or(OutputLimits{});
        // Built for this call, so a rerun after recovery gets the conversation's environment at that time.
        auto env = runtime.env();
        ToolCallApi api(runtime, call.at("id").get<std::string>(), limits, env ? *env : nullptr);
        Result<ToolExecutionResult> executed = env ? (tool.execute ? tool.execute(args, api)
                                                                   : Result<ToolExecutionResult>(std::unexpected(Error{"tool_error", "Tool has no implementation"})))
                                                   : Result<ToolExecutionResult>(std::unexpected(env.error()));
        std::string ending = "completed";
        std::string failure;
        ToolExecutionResult result;
        if (!executed) {
            if (runtime.signal().aborted()) {
                for (const auto& waiter : api.abandon()) {
                    api.reject(waiter, executed.error());
                }
                return std::unexpected(executed.error());
            }
            result.isError = true;
            result.diagnostics.push_back(m_results.diagnostic("error", "tool_error", executed.error().message));
            // A throw, from `execute` or from building the environment, ends the task `failed`, which cancels what the
            // call owned; it no longer supervises it. The error text is already in the result entry.
            ending = "failed";
            failure = "Tool " + call.at("name").get<std::string>() + " threw";
        } else {
            result = *executed;
        }
        // Details still waiting for a progress commit settle with the terminal commit, the final flush.
        const auto pending = api.finish();
        auto settled = finalResult(runtime, call, result, api, limits);
        Result<void> done = settled ? settle(runtime, call, ending, failure, [&](const Json*) { return *settled; })
                                    : Result<void>(std::unexpected(settled.error()));
        for (const auto& waiter : pending) {
            if (done) {
                api.resolve(waiter);
            } else {
                api.reject(waiter, done.error());
            }
        }
        return done;
    }

    /**
     * The settled result: the tool's result with the retained output and last details as fallbacks, its diagnostics
     * after those reported through the api, `afterTool` applied, and explicit text bounded, with the harness's
     * truncation diagnostic last.
     */
    Result<ToolExecutionResult> finalResult(ITaskRuntime& runtime, const Json& call, const ToolExecutionResult& result,
                                            ToolCallApi& api, const OutputLimits& limits) const {
        std::vector<ToolDiagnostic> harness;
        std::optional<BoundedOutput> retained;
        Json content = Json::array();
        if (result.content) {
            content = *result.content;
        } else {
            retained = api.retainedOutput();
            if (!retained->text.empty()) {
                content = Json::array({Json::object({{"type", "text"}, {"text", retained->text}})});
            }
        }
        ToolExecutionResult final = result;
        final.content = content;
        if (!final.details) {
            final.details = api.reportedDetails();
        }
        std::vector<ToolDiagnostic> diagnostics = api.reportedDiagnostics();
        diagnostics.insert(diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
        final.diagnostics = diagnostics;
        auto hooked = runtime.eachHook("afterTool", [&](const HookHandler& hook) -> Result<void> {
            auto decision = hook(Json::object({{"call", call}, {"result", m_results.toJson(final)}}), runtime);
            if (!decision) {
                return std::unexpected(decision.error());
            }
            if (*decision) {
                final = m_results.fromJson(**decision);
            }
            return {};
        });
        if (!hooked) {
            return std::unexpected(hooked.error());
        }
        // The retained output's truncation applies only while afterTool kept that content.
        if (retained && retained->droppedBytes > 0 && final.content == std::optional<Json>(content)) {
            harness.push_back(truncated(retained->droppedLines, retained->droppedBytes, limits.retain));
        }
        const BoundedContent bounded = boundContent(final.content ? *final.content : Json::array(), limits);
        if (bounded.droppedBytes > 0) {
            harness.push_back(truncated(bounded.droppedLines, bounded.droppedBytes, limits.retain));
        }
        final.content = bounded.content;
        final.diagnostics.insert(final.diagnostics.end(), harness.begin(), harness.end());
        return final;
    }

    /**
     * Commits the tool's terminal state: appends its result entry, marks its slot done, and ends with the entry id.
     * `build` receives the slot so interruption and abort can report the durable partial output. `ending` is
     * "completed", "aborted" or "failed" (with `failure` as the error message).
     */
    Result<void> settle(ITaskRuntime& runtime, const Json& call, const std::string& ending, const std::string& failure,
                        const std::function<ToolExecutionResult(const Json* slot)>& build) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            DocAddressArgs address;
            address.owner = runtime.conversationId();
            auto live = tx.doc(m_documents.live(), address);
            if (!live) {
                return std::unexpected(live.error());
            }
            Json* slot = m_live.toolSlot(**live, runtime.taskId());
            const ToolExecutionResult result = build(slot);
            auto entry = m_results.append(tx, runtime.conversationId(), call, result, runtime.now());
            if (!entry) {
                return std::unexpected(entry.error());
            }
            const std::int64_t entryId = entry->at("id").get<std::int64_t>();
            if (slot != nullptr) {
                m_live.finishSlot(*slot, entryId);
            }
            if (ending == "aborted") {
                return std::optional<Json>(Json::object({{"status", "terminal"},
                                                         {"outcome", Json::object({{"status", "aborted"}, {"result", Json::object({{"entryId", entryId}})}})}}));
            }
            if (ending == "failed") {
                return std::optional<Json>(Json::object({{"status", "terminal"},
                                                         {"outcome", Json::object({{"status", "failed"},
                                                                                   {"error", Json::object({{"message", failure}})},
                                                                                   {"result", Json::object({{"entryId", entryId}})}})}}));
            }
            Json completed = Json::object({{"entryId", entryId}});
            if (result.control) {
                completed["control"] = *result.control;
            }
            return std::optional<Json>(Json::object({{"status", "terminal"},
                                                     {"outcome", Json::object({{"status", "completed"}, {"result", completed}})}}));
        });
    }

    // ─── Results ────────────────────────────────────────────────────────────

    /** An error result from the slot's durable partial output, details and diagnostics. */
    ToolExecutionResult fromSlot(const Json* slot, const std::string& code, const std::string& message) const {
        ToolExecutionResult result;
        std::vector<ToolDiagnostic> diagnostics;
        if (slot != nullptr && slot->contains("diagnostics")) {
            for (const Json& item : slot->at("diagnostics")) {
                diagnostics.push_back(m_results.diagnosticFromJson(item));
            }
        }
        const std::int64_t droppedBytes = slot != nullptr ? slot->value("droppedBytes", std::int64_t(0)) : 0;
        if (droppedBytes > 0) {
            diagnostics.push_back(truncated(slot->value("droppedLines", std::int64_t(0)), droppedBytes, std::nullopt));
        }
        diagnostics.push_back(m_results.diagnostic("error", code, message));
        const std::string output = slot != nullptr ? slot->value("output", std::string()) : std::string();
        result.content = output.empty() ? Json::array() : Json::array({Json::object({{"type", "text"}, {"text", output}})});
        result.isError = true;
        if (slot != nullptr && slot->contains("details")) {
            result.details = slot->at("details");
        }
        result.diagnostics = diagnostics;
        return result;
    }

    /** The harness's truncation diagnostic; `retain` is unknown when rebuilt from a slot after recovery. */
    ToolDiagnostic truncated(std::int64_t droppedLines, std::int64_t droppedBytes, const std::optional<std::string>& retain) const {
        const std::string kept = retain ? std::string(" to its ") + (*retain == "head" ? "beginning" : "end") : std::string();
        return m_results.diagnostic("warn", "truncated", "Output truncated" + kept + ": " + std::to_string(droppedLines) +
                                                             " lines, " + std::to_string(droppedBytes) + " bytes dropped");
    }

    /**
     * Bounds the text of result content. When the joined text exceeds the limits, the text items are replaced by one
     * bounded item at the position of the first (head) or last (tail) text item; other content is kept.
     */
    BoundedContent boundContent(const Json& content, const OutputLimits& limits) const {
        std::vector<std::size_t> texts;
        std::string joined;
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i].value("type", std::string()) == "text") {
                texts.push_back(i);
                joined += content[i].value("text", std::string());
            }
        }
        const OutputSlice bounded = m_bounder.bound(joined, limits);
        if (bounded.droppedBytes == 0) {
            return BoundedContent{content, 0, 0};
        }
        const std::size_t keep = limits.retain == "head" ? texts.front() : texts.back();
        Json result = Json::array();
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i].value("type", std::string()) != "text") {
                result.push_back(content[i]);
            } else if (i == keep) {
                Json item = content[i];
                item["text"] = bounded.text;
                result.push_back(item);
            }
        }
        return BoundedContent{result, bounded.droppedBytes, bounded.droppedLines};
    }

    BuiltinDocuments m_documents;
    EntryKinds m_kinds;
    LiveEditor m_live;
    OutputBounder m_bounder;
    SchemaValidator m_validator;
    ToolResults m_results;
};
