module;

#include <cstdint>

export module pi.support.generation_task_definition;

import std;
export import pi.durable.task_definition;
export import pi.support.agent_configurator;
export import pi.support.assistant_entries;
export import pi.support.builtin_documents;
export import pi.support.compaction_planner;
export import pi.support.entry_kinds;
export import pi.support.inbox_boundary;
export import pi.support.live_editor;
export import pi.support.model_requests;
export import pi.support.progress_publisher;
export import pi.support.prompt_planner;
export import pi.support.response_classifier;
export import pi.support.run_starter;
export import pi.support.tool_results;
export import pi.support.tool_task_definition;

/**
 * The built-in generation task (`pi.generation`): prepares the positional system prompt and tool loadout, requests the
 * model, retries, and classifies the response; a tool-calling answer starts a tool round that the generation owns and
 * waits for. The run's inputs live in `pi.live.run`. Port of packages/durable/src/harness/generation.ts.
 *
 * Input `{}`. Checkpoints: `{phase: "prepare", attempt, compacted?, overflow?}`, `{phase: "request", attempt, compacted?,
 * model, thinkingLevel, streamOptions, cutoff}`, `{phase: "retry", attempt, compacted?, until}`, `{phase: "poll", attempt, compacted?,
 * model, cutoff, handle, pollAt}` (a deferred provider response, fetched again at `pollAt`) and `{phase: "tools", assistant,
 * tools, pending}`. Result `{entryId}`.
 */
export class GenerationTaskDefinition {
public:
    static constexpr std::int64_t kPartialIntervalMs = 100;
    static constexpr std::int64_t kDefaultPollAfterMs = 5000;

    std::shared_ptr<TaskDefinition> build() const {
        auto definition = std::make_shared<TaskDefinition>();
        definition->name = m_runs.generationKind();
        definition->version = 1;
        definition->initial = [self = *this](const Json&) { return self.m_runs.generationCheckpoint(); };
        definition->phases["prepare"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.prepare(task, runtime); };
        definition->phases["request"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.request(task, runtime); };
        definition->phases["retry"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.retry(task, runtime); };
        definition->phases["poll"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.poll(task, runtime); };
        definition->phases["tools"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.tools(task, runtime); };
        definition->abort = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.abort(task, runtime); };
        return definition;
    }

private:
    using State = Result<std::optional<Json>>;

    // ─── Phases ─────────────────────────────────────────────────────────────

    /**
     * Renders the system prompt and tool loadout and appends the positional `pi.system` entries they need, then moves to
     * `request`. The agent and settings resolved here are fixed for this request. Only the harness writes to a busy
     * conversation, so the transcript read here is still the tail at the commit.
     */
    Result<void> prepare(const Json& task, ITaskRuntime& runtime) const {
        const std::int64_t conversationId = runtime.conversationId();
        auto agent = runtime.agent();
        if (!agent) {
            return std::unexpected(agent.error());
        }
        const ResolvedSettings settings = runtime.settings();
        const std::shared_ptr<const AgentSnapshot> snapshot = (*agent)->snapshot();
        if (!snapshot->model) {
            return failNoModel(runtime, "No model is configured");
        }
        auto model = m_requests.find(runtime.models(), *snapshot->model);
        if (!model) {
            return failNoModel(runtime, model.error().message);
        }
        const Json& checkpoint = task.at("state").at("checkpoint");
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>();
        const std::optional<std::int64_t> compacted = optionalId(checkpoint, "compacted");
        if (compacted && checkpoint.contains("overflow")) {
            auto outcomes = runtime.outcomes({*compacted});
            if (!outcomes) {
                return std::unexpected(outcomes.error());
            }
            const Json& outcome = (*outcomes)[0];
            if (outcome.at("status") != "completed" || !outcome.at("result").contains("entryId")) {
                return failModelError(runtime, checkpoint.at("overflow").get<std::string>());
            }
        }
        auto view = runtime.context(conversationId);
        if (!view) {
            return std::unexpected(view.error());
        }
        const PromptPlanner::Sections shown = m_prompt.replaySections(view->at("messages"));
        PromptInput input;
        input.conversationId = conversationId;
        input.agent = snapshot;
        for (const auto& item : shown) {
            input.shown[item.first] = item.second;
        }
        auto desired = m_prompt.render((*agent)->sections(), input, shown, [&](const Error& error) { runtime.report(error); }, &runtime.signal());
        if (!desired) {
            return std::unexpected(desired.error());
        }
        const std::vector<Json> entries = m_prompt.plan(*view, *desired, snapshot->tools, runtime.now());
        std::string threshold;
        if (!compacted) {
            threshold = thresholdCompaction(*view, entries, model->contextWindow, settings.compaction);
        }
        if (threshold == "blocking") {
            // Compact first and prepare again; the transcript is unchanged until the compaction appends.
            return runtime.commit([&](Transaction& tx, const Json&) -> State {
                auto child = m_compaction.createCompaction(tx, conversationId, Json::object({{"reason", "threshold"}}), runtime.taskId());
                if (!child) {
                    return std::unexpected(child.error());
                }
                return waiting(Json::object({{"phase", "prepare"}, {"attempt", attempt}, {"compacted", *child}}), {*child});
            });
        }
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto newest = tx.scanEntries(EntryQuery{conversationId, std::nullopt, std::nullopt}, 1, std::nullopt);
            if (!newest) {
                return std::unexpected(newest.error());
            }
            std::optional<std::int64_t> cutoff;
            if (!newest->items.empty()) {
                cutoff = newest->items[0].at("id").get<std::int64_t>();
            }
            for (const Json& entry : entries) {
                auto appended = tx.appendEntry(conversationId, entry);
                if (!appended) {
                    return std::unexpected(appended.error());
                }
                cutoff = appended->at("id").get<std::int64_t>();
            }
            if (!cutoff) {
                return std::unexpected(Error{"durable_error", "Conversation " + std::to_string(conversationId) + " has no entries to send"});
            }
            // Checked in this commit, so a compaction admitted during preparation counts.
            if (threshold == "background") {
                auto live = liveOf(tx, conversationId);
                if (!live) {
                    return std::unexpected(live.error());
                }
                if (!(*live)->contains("compactions")) {
                    if (auto created = m_compaction.createCompaction(tx, conversationId, Json::object({{"reason", "threshold"}})); !created) {
                        return std::unexpected(created.error());
                    }
                }
            }
            Json request = Json::object({{"phase", "request"}, {"attempt", attempt}});
            if (compacted) {
                request["compacted"] = *compacted;
            }
            request["model"] = *snapshot->model;
            request["thinkingLevel"] = snapshot->thinkingLevel;
            request["streamOptions"] = settings.stream;
            request["cutoff"] = *cutoff;
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", request}}));
        });
    }

    Result<void> request(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>();
        const std::int64_t conversationId = runtime.conversationId();
        const std::int64_t cutoff = checkpoint.at("cutoff").get<std::int64_t>();
        auto started = runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto converted = m_assistants.convertPartial(tx, **live, conversationId); !converted) {
                return std::unexpected(converted.error());
            }
            (**live)["generation"] = Json::object({{"attempt", attempt}});
            return std::optional<Json>();
        });
        if (!started) {
            return started;
        }
        auto model = m_requests.find(runtime.models(), checkpoint.at("model"));
        if (!model) {
            return failNoModel(runtime, model.error().message);
        }
        auto view = runtime.context(conversationId, cutoff);
        if (!view) {
            return std::unexpected(view.error());
        }
        Json messages = view->at("messages");
        auto hooked = runtime.eachHook("beforeRequest", [&](const HookHandler& hook) -> Result<void> {
            auto replaced = hook(Json::object({{"messages", messages}}), runtime);
            if (!replaced) {
                return std::unexpected(replaced.error());
            }
            if (*replaced && (*replaced)->contains("messages")) {
                messages = (*replaced)->at("messages");
            }
            return {};
        });
        if (!hooked) {
            return hooked;
        }
        const StreamOptions options = m_requests.options(checkpoint.value("streamOptions", Json::object()),
                                                         checkpoint.value("thinkingLevel", std::string("off")), runtime.signal());
        auto message = streamResponse(runtime, *model, messages, options, attempt);
        if (!message) {
            return std::unexpected(message.error());
        }
        return classify(runtime, checkpoint, view->at("messages"), *message);
    }

    Result<void> retry(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        if (auto slept = runtime.sleep(checkpoint.at("until").get<std::int64_t>()); !slept) {
            return slept;
        }
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>() + 1;
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, runtime.conversationId());
            if (!live) {
                return std::unexpected(live.error());
            }
            (**live)["generation"] = Json::object({{"attempt", attempt}});
            Json next = Json::object({{"phase", "prepare"}, {"attempt", attempt}});
            if (checkpoint.contains("compacted")) {
                next["compacted"] = checkpoint.at("compacted");
            }
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", next}}));
        });
    }

    /** Waits until `pollAt`, fetches the deferred response again and classifies what comes back. */
    Result<void> poll(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        auto model = m_requests.find(runtime.models(), checkpoint.at("model"));
        if (!model) {
            return failNoModel(runtime, model.error().message);
        }
        if (auto slept = runtime.sleep(checkpoint.at("pollAt").get<std::int64_t>()); !slept) {
            return slept;
        }
        const StreamOptions options = m_requests.options(Json::object(), "off", runtime.signal());
        auto message = m_requests.fetchDeferred(*runtime.models(), *model, checkpoint.at("handle"), options);
        if (!message) {
            return std::unexpected(message.error());
        }
        return classify(runtime, checkpoint, Json(), *message);
    }

    Result<void> tools(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        const std::int64_t assistant = checkpoint.at("assistant").get<std::int64_t>();
        const Json& pending = checkpoint.at("pending");
        if (pending.empty()) {
            return finishToolRound(runtime, assistant, checkpoint.at("tools"));
        }
        // Sequential round: start the next call and wait for it.
        const std::string next = pending[0].get<std::string>();
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, runtime.conversationId());
            if (!live) {
                return std::unexpected(live.error());
            }
            auto taskId = createToolTask(tx, runtime, assistant, next);
            if (!taskId) {
                return std::unexpected(taskId.error());
            }
            if (live.value()->contains("tools")) {
                for (Json& slot : (**live)["tools"]) {
                    if (slot.value("callId", std::string()) == next && !slot.contains("taskId")) {
                        slot["taskId"] = *taskId;
                        break;
                    }
                }
            }
            Json tools = checkpoint.at("tools");
            tools.push_back(*taskId);
            Json rest = Json::array();
            for (std::size_t i = 1; i < pending.size(); ++i) {
                rest.push_back(pending[i]);
            }
            return waiting(Json::object({{"phase", "tools"}, {"assistant", assistant}, {"tools", tools}, {"pending", rest}}), {*taskId});
        });
    }

    Result<void> abort(const Json& task, ITaskRuntime& runtime) const {
        const Json& checkpoint = task.at("state").at("checkpoint");
        const std::int64_t conversationId = runtime.conversationId();
        if (checkpoint.at("phase") == "poll") {
            cancelDeferred(runtime, checkpoint);
        }
        // Runs after the round's tool tasks are terminal; calls never started get `aborted` results.
        std::vector<Json> unstarted;
        if (checkpoint.at("phase") == "tools") {
            auto calls = readCalls(runtime, checkpoint.at("assistant").get<std::int64_t>(), checkpoint.at("pending"));
            if (!calls) {
                return std::unexpected(calls.error());
            }
            unstarted = *calls;
        }
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto converted = m_assistants.convertPartial(tx, **live, conversationId); !converted) {
                return std::unexpected(converted.error());
            }
            for (const Json& call : unstarted) {
                const ToolExecutionResult result = m_results.harnessError("aborted", "Tool " + call.at("name").get<std::string>() + " was aborted");
                if (auto appended = m_results.append(tx, conversationId, call, result, runtime.now()); !appended) {
                    return std::unexpected(appended.error());
                }
            }
            if (auto ended = m_live.endRun(tx, **live, runtime.taskId(), Json::object({{"status", "unanswered"}, {"reason", "aborted"}})); !ended) {
                return std::unexpected(ended.error());
            }
            return std::optional<Json>(Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "aborted"}})}}));
        });
    }

    /** Cancels the provider's deferred response of a `poll` checkpoint; a failure is reported, the abort goes on. */
    void cancelDeferred(ITaskRuntime& runtime, const Json& checkpoint) const {
        auto model = m_requests.find(runtime.models(), checkpoint.at("model"));
        if (!model) {
            return;
        }
        const StreamOptions options = m_requests.options(Json::object(), "off", runtime.signal());
        if (auto cancelled = m_requests.cancelDeferred(*runtime.models(), *model, checkpoint.at("handle"), options); !cancelled) {
            runtime.report(cancelled.error());
        }
    }

    // ─── Requests ───────────────────────────────────────────────────────────

    /**
     * Streams one request and returns the terminal message. Partials commit at most once per publisher interval with
     * one commit in flight; stopping the publisher before returning awaits that commit, so no stale partial lands after
     * the outcome.
     */
    Result<Json> streamResponse(ITaskRuntime& runtime, const Model& model, const Json& messages, const StreamOptions& options,
                                std::int64_t attempt) const {
        std::mutex mutex;
        std::optional<Json> pending;
        ProgressPublisher publisher(
            [&]() -> Result<std::int64_t> {
                std::optional<Json> partial;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    partial = std::exchange(pending, std::nullopt);
                }
                if (!partial) {
                    return 0;
                }
                auto committed = runtime.commit([&](Transaction& tx, const Json&) -> State {
                    auto live = liveOf(tx, runtime.conversationId());
                    if (!live) {
                        return std::unexpected(live.error());
                    }
                    if (!(*live)->contains("generation")) {
                        (**live)["generation"] = Json::object({{"attempt", attempt}});
                    }
                    (**live)["generation"]["message"] = *partial;
                    return std::optional<Json>();
                });
                if (!committed) {
                    return std::unexpected(committed.error());
                }
                return static_cast<std::int64_t>(partial->dump().size());
            },
            [&](const Error& error) {
                // Failures after an abort mark or close are expected; the committed state stays consistent.
                if (!runtime.signal().aborted()) {
                    runtime.report(error);
                }
            });
        auto stream = m_requests.open(*runtime.models(), model, messages, options);
        if (!stream) {
            (void)publisher.stop();
            return std::unexpected(stream.error());
        }
        while (auto event = (*stream)->next()) {
            // A partial without content, such as the opening `start` event, shows nothing.
            if (event->type == AssistantEventType::Done || event->type == AssistantEventType::Error || !event->partial ||
                event->partial->content.empty()) {
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                pending = m_requests.toJson(*event->partial);
            }
            publisher.mark();
        }
        (void)publisher.stop();
        auto final = (*stream)->result();
        if (!final) {
            return std::unexpected(Error{"model_error", "The model stream ended without a result"});
        }
        return m_requests.toJson(*final);
    }

    /** Classifies a terminal provider message in one commit that also clears the partial. */
    Result<void> classify(ITaskRuntime& runtime, const Json& checkpoint, const Json& contextMessages, const Json& message) const {
        // An abort mark or close: the abort invocation or the reopened run handles the committed state.
        if (runtime.signal().aborted()) {
            return std::unexpected(Error{"aborted", "The operation was aborted"});
        }
        const std::int64_t conversationId = runtime.conversationId();
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>();
        const std::optional<std::int64_t> compacted = optionalId(checkpoint, "compacted");
        const std::int64_t cutoff = checkpoint.at("cutoff").get<std::int64_t>();
        const std::string stop = message.value("stopReason", std::string());
        if (stop == "deferred" && message.contains("deferred")) {
            return pollLater(runtime, checkpoint, message.at("deferred"));
        }
        auto hooked = runtime.eachHook("afterResponse", [&](const HookHandler& hook) -> Result<void> {
            auto handled = hook(message, runtime);
            return handled ? Result<void>() : std::unexpected(handled.error());
        });
        if (!hooked) {
            return hooked;
        }
        std::vector<Json> calls;
        for (const Json& block : message.at("content")) {
            if (block.value("type", std::string()) == "toolCall") {
                calls.push_back(block);
            }
        }
        if (stop == "toolUse" && !calls.empty()) {
            Json messages = contextMessages;
            if (messages.is_null()) {
                // A polled response has no request context at hand; the offered tools are those the request saw.
                auto view = runtime.context(conversationId, cutoff);
                if (!view) {
                    return std::unexpected(view.error());
                }
                messages = view->at("messages");
            }
            return startToolRound(runtime, messages, message, calls);
        }
        if (stop == "stop" || stop == "length" || stop == "toolUse") {
            return answer(runtime, message);
        }
        // The retry and compaction policies govern the next attempt, so they are read now rather than pinned at preparation.
        const ResolvedSettings settings = runtime.settings();
        const bool overflow = stop == "error" && m_classifier.contextOverflow(message);
        if (overflow && !compacted && settings.compaction.enabled) {
            auto view = runtime.context(conversationId, cutoff);
            if (!view) {
                return std::unexpected(view.error());
            }
            if (m_compaction.selectCut(*view, settings.compaction.keepRecentTokens)) {
                return compactForOverflow(runtime, attempt, message);
            }
        }
        return endAttempt(runtime, checkpoint, message, overflow, settings.retry);
    }

    /** Commits the `poll` checkpoint of a deferred response; a still pending one polls strictly later than before. */
    Result<void> pollLater(ITaskRuntime& runtime, const Json& checkpoint, const Json& handle) const {
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>();
        std::int64_t pollAt = runtime.now() + handle.value("pollAfterMs", kDefaultPollAfterMs);
        if (const auto previous = optionalId(checkpoint, "pollAt")) {
            pollAt = std::max(pollAt, *previous + 1);
        }
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, runtime.conversationId());
            if (!live) {
                return std::unexpected(live.error());
            }
            (**live)["generation"] = Json::object({{"attempt", attempt}, {"deferred", Json::object({{"pollAt", pollAt}})}});
            Json next = Json::object({{"phase", "poll"}, {"attempt", attempt}});
            if (checkpoint.contains("compacted")) {
                next["compacted"] = checkpoint.at("compacted");
            }
            next["model"] = checkpoint.at("model");
            next["cutoff"] = checkpoint.at("cutoff");
            next["handle"] = handle;
            next["pollAt"] = pollAt;
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", next}}));
        });
    }

    /** Appends the overflowing answer and waits for a blocking compaction the generation owns. */
    Result<void> compactForOverflow(ITaskRuntime& runtime, std::int64_t attempt, const Json& message) const {
        const std::int64_t conversationId = runtime.conversationId();
        const std::string text = message.value("errorMessage", std::string("Context overflow"));
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto appended = m_assistants.append(tx, conversationId, message); !appended) {
                return std::unexpected(appended.error());
            }
            (*live)->erase("generation");
            auto child = m_compaction.createCompaction(tx, conversationId, Json::object({{"reason", "overflow"}}), runtime.taskId());
            if (!child) {
                return std::unexpected(child.error());
            }
            return waiting(Json::object({{"phase", "prepare"}, {"attempt", attempt}, {"compacted", *child}, {"overflow", text}}), {*child});
        });
    }

    /** Appends a failed or truncated answer and either schedules the next attempt or fails the run. */
    Result<void> endAttempt(ITaskRuntime& runtime, const Json& checkpoint, const Json& message, bool overflow,
                            const ConversationRetryPolicy& policy) const {
        const std::int64_t conversationId = runtime.conversationId();
        const std::int64_t attempt = checkpoint.at("attempt").get<std::int64_t>();
        const std::string stop = message.value("stopReason", std::string());
        // An overflow is never retried: only a compaction can make the next request fit.
        const bool retry = stop == "error" && !overflow && m_classifier.retryable(message) && policy.enabled && attempt <= policy.maxRetries;
        const std::int64_t until = retry ? runtime.now() + m_classifier.retryDelayMs(policy, attempt) : 0;
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto appended = m_assistants.append(tx, conversationId, message); !appended) {
                return std::unexpected(appended.error());
            }
            if (retry) {
                (**live)["generation"] = Json::object({{"attempt", attempt}, {"retry", Json::object({{"at", until}, {"error", message.value("errorMessage", std::string())}})}});
                Json next = Json::object({{"phase", "retry"}, {"attempt", attempt}});
                if (checkpoint.contains("compacted")) {
                    next["compacted"] = checkpoint.at("compacted");
                }
                next["until"] = until;
                return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", next}}));
            }
            const std::string text = message.contains("errorMessage") ? message.at("errorMessage").get<std::string>()
                                                                       : "Model response ended with stop reason " + stop;
            if (auto ended = m_live.endRun(tx, **live, runtime.taskId(),
                                           Json::object({{"status", "unanswered"}, {"reason", "model_error"}, {"detail", text}}));
                !ended) {
                return std::unexpected(ended.error());
            }
            return failed(text, "model_error");
        });
    }

    /**
     * A final answer; the final boundary places queued items. The first `onYield` continuation appends a user message
     * and hands the run to a successor generation, but only when the boundary selected no user item and no reset.
     * Otherwise the run's inputs settle `done`, and selected user items start the next run.
     */
    Result<void> answer(ITaskRuntime& runtime, const Json& message) const {
        std::optional<Json> continuation;
        auto hooked = runtime.eachHook("onYield", [&](const HookHandler& hook) -> Result<void> {
            if (continuation) {
                return {};
            }
            auto decision = hook(message, runtime);
            if (!decision) {
                return std::unexpected(decision.error());
            }
            if (*decision && (*decision)->contains("continue")) {
                continuation = (*decision)->at("continue");
            }
            return {};
        });
        if (!hooked) {
            return hooked;
        }
        const std::int64_t conversationId = runtime.conversationId();
        const ResolvedSettings settings = runtime.settings();
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            // Queue modes are read on the session line, when the boundary is decided.
            auto boundary = m_boundary.prepare(tx, conversationId, settings.steeringMode, settings.followUpMode);
            if (!boundary) {
                return std::unexpected(boundary.error());
            }
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            auto entry = m_assistants.append(tx, conversationId, message);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            const std::int64_t entryId = entry->at("id").get<std::int64_t>();
            const Json result = completed(Json::object({{"entryId", entryId}}));
            auto applied = m_boundary.apply(tx, *boundary, "final", runtime.now());
            if (!applied) {
                return std::unexpected(applied.error());
            }
            if (continuation && applied->users.empty() && !applied->reset) {
                const Json user = Json::object({{"role", "user"}, {"content", *continuation}, {"timestamp", runtime.now()}});
                if (auto appended = tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.user()}, {"model", Json::array({user})}})); !appended) {
                    return std::unexpected(appended.error());
                }
                auto successor = m_runs.createGeneration(tx, conversationId);
                if (!successor) {
                    return std::unexpected(successor.error());
                }
                m_runs.handOver(**live, runtime.taskId(), *successor);
                (*live)->erase("generation");
                return std::optional<Json>(result);
            }
            if (auto ended = m_live.endRun(tx, **live, runtime.taskId(), Json::object({{"status", "done"}, {"answer", entryId}})); !ended) {
                return std::unexpected(ended.error());
            }
            if (!applied->users.empty()) {
                if (auto started = m_runs.startRun(tx, conversationId, **live, applied->users); !started) {
                    return std::unexpected(started.error());
                }
            }
            return std::optional<Json>(result);
        });
    }

    // ─── Tool rounds ────────────────────────────────────────────────────────

    /**
     * Appends the tool-calling answer and starts its tool round in one commit. A call to a tool the request did not
     * offer gets its `tool_unavailable` result here; every other call gets a tool task owned by the generation, only the
     * first one now when the round is sequential. The generation then waits for them in its `tools` phase, keeping the run.
     */
    Result<void> startToolRound(ITaskRuntime& runtime, const Json& contextMessages, const Json& message, const std::vector<Json>& calls) const {
        const std::int64_t conversationId = runtime.conversationId();
        std::set<std::string> offered;
        for (const Json& tool : m_prompt.currentTools(contextMessages)) {
            offered.insert(tool.at("name").get<std::string>());
        }
        // Read as the round starts; a tool is resolved as its tool task resolves it.
        auto agent = runtime.agent();
        if (!agent) {
            return std::unexpected(agent.error());
        }
        bool sequential = runtime.settings().toolExecution == "sequential";
        for (const Json& call : calls) {
            const std::string name = call.at("name").get<std::string>();
            for (const ToolRegistration& tool : (*agent)->snapshot()->tools) {
                if (offered.contains(name) && tool.name == name && tool.executionMode == std::optional<std::string>("sequential")) {
                    sequential = true;
                }
            }
        }
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            auto entry = m_assistants.append(tx, conversationId, message);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            Json slots = Json::array();
            Json taskIds = Json::array();
            Json pending = Json::array();
            for (const Json& call : calls) {
                const std::string name = call.at("name").get<std::string>();
                const std::string callId = call.at("id").get<std::string>();
                if (!offered.contains(name)) {
                    auto result = m_results.append(tx, conversationId, call, m_results.harnessError("tool_unavailable", "Tool " + name + " is not available"), runtime.now());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    slots.push_back(Json::object({{"callId", callId}, {"name", name}, {"status", "done"}, {"entry", result->at("id")}}));
                    continue;
                }
                if (sequential && !taskIds.empty()) {
                    pending.push_back(callId);
                    slots.push_back(Json::object({{"callId", callId}, {"name", name}, {"status", "pending"}}));
                    continue;
                }
                auto taskId = createToolTask(tx, runtime, entry->at("id").get<std::int64_t>(), callId);
                if (!taskId) {
                    return std::unexpected(taskId.error());
                }
                taskIds.push_back(*taskId);
                slots.push_back(Json::object({{"callId", callId}, {"name", name}, {"taskId", *taskId}, {"status", "pending"}}));
            }
            (*live)->erase("generation");
            (**live)["tools"] = slots;
            std::vector<std::int64_t> on;
            for (const Json& id : taskIds) {
                on.push_back(id.get<std::int64_t>());
            }
            return waiting(Json::object({{"phase", "tools"}, {"assistant", entry->at("id")}, {"tools", taskIds}, {"pending", pending}}), on);
        });
    }

    /**
     * The round's tools are terminal: applies their controls and either ends the run at the final boundary
     * (`terminate`, `handoff`, or a queued reset) or hands it to the next generation at the `postTools` boundary.
     */
    Result<void> finishToolRound(ITaskRuntime& runtime, std::int64_t assistant, const Json& tools) const {
        const std::int64_t conversationId = runtime.conversationId();
        std::vector<std::int64_t> ids;
        for (const Json& id : tools) {
            ids.push_back(id.get<std::int64_t>());
        }
        auto outcomes = runtime.outcomes(ids);
        if (!outcomes) {
            return std::unexpected(outcomes.error());
        }
        std::map<std::int64_t, Json> controls;
        std::vector<Json> controlsInOrder;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const Json& outcome = (*outcomes)[i];
            const bool completedWithControl = outcome.at("status") == "completed" && outcome.at("result").contains("control");
            if (completedWithControl) {
                controls[ids[i]] = outcome.at("result").at("control");
                controlsInOrder.push_back(outcome.at("result").at("control"));
            }
        }
        auto liveValue = runtime.snapshot(m_documents.live(), ownerArgs(conversationId));
        if (!liveValue) {
            return std::unexpected(liveValue.error());
        }
        const Json slots = *liveValue && (*liveValue)->contains("tools") ? (*liveValue)->at("tools") : Json::array();
        Json results = Json::array();
        bool terminate = !slots.empty();
        for (const Json& slot : slots) {
            if (slot.contains("entry")) {
                results.push_back(slot.at("entry"));
            }
            const bool asked = slot.contains("taskId") && controls.contains(slot.at("taskId").get<std::int64_t>()) &&
                               controls.at(slot.at("taskId").get<std::int64_t>()).value("terminate", false);
            terminate = terminate && asked;
        }
        auto hooked = runtime.eachHook("afterTools", [&](const HookHandler& hook) -> Result<void> {
            auto handled = hook(Json::object({{"assistant", assistant}, {"results", results}}), runtime);
            return handled ? Result<void>() : std::unexpected(handled.error());
        });
        if (!hooked) {
            return hooked;
        }
        // Every call of the round, including those answered without a task, must ask to terminate.
        std::vector<std::string> added;
        std::optional<std::string> handoff;
        for (const Json& control : controlsInOrder) {
            if (control.contains("addTools")) {
                for (const Json& name : control.at("addTools")) {
                    added.push_back(name.get<std::string>());
                }
            }
            // The last handoff in call order wins.
            if (control.contains("handoff")) {
                handoff = control.at("handoff").get<std::string>();
            }
        }
        const ResolvedSettings settings = runtime.settings();
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto boundary = m_boundary.prepare(tx, conversationId, settings.steeringMode, settings.followUpMode);
            if (!boundary) {
                return std::unexpected(boundary.error());
            }
            if (!added.empty()) {
                if (auto addedTools = m_configurator.addTools(tx, conversationId, added); !addedTools) {
                    return std::unexpected(addedTools.error());
                }
            }
            auto live = liveOf(tx, conversationId);
            if (!live) {
                return std::unexpected(live.error());
            }
            const std::int64_t now = runtime.now();
            if (terminate || handoff) {
                if (auto ended = endToolRun(tx, runtime, **live, *boundary, assistant, handoff, now); !ended) {
                    return std::unexpected(ended.error());
                }
            } else if (auto continued = continueToolRun(tx, runtime, **live, *boundary, now); !continued) {
                return std::unexpected(continued.error());
            }
            return std::optional<Json>(completed(Json::object({{"entryId", assistant}})));
        });
    }

    /** `terminate` or `handoff`: the run ends at the final boundary; a handoff starts a new context first. */
    Result<void> endToolRun(Transaction& tx, ITaskRuntime& runtime, Json& live, Boundary& boundary, std::int64_t assistant,
                            const std::optional<std::string>& handoff, std::int64_t now) const {
        const std::int64_t conversationId = runtime.conversationId();
        if (handoff) {
            const Json message = Json::object({{"role", "user"}, {"content", *handoff}, {"timestamp", now}});
            auto entry = tx.appendEntry(conversationId, Json::object({{"kind", m_kinds.reset()}, {"head", "self"}, {"model", Json::array({message})}}));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            boundary.head = entry->at("id").get<std::int64_t>();
        }
        auto applied = m_boundary.apply(tx, boundary, "final", now);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (auto ended = m_live.endRun(tx, live, runtime.taskId(), Json::object({{"status", "done"}, {"answer", assistant}})); !ended) {
            return ended;
        }
        return applied->users.empty() ? Result<void>() : m_runs.startRun(tx, conversationId, live, applied->users);
    }

    /** No control ended the run: the postTools boundary decides between a reset and the next generation. */
    Result<void> continueToolRun(Transaction& tx, ITaskRuntime& runtime, Json& live, Boundary& boundary, std::int64_t now) const {
        const std::int64_t conversationId = runtime.conversationId();
        auto applied = m_boundary.apply(tx, boundary, "postTools", now);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (applied->reset) {
            // The queued reset cut the run's context before an answer.
            if (auto ended = m_live.endRun(tx, live, runtime.taskId(), Json::object({{"status", "unanswered"}, {"reason", "reset"}})); !ended) {
                return ended;
            }
            return applied->users.empty() ? Result<void>() : m_runs.startRun(tx, conversationId, live, applied->users);
        }
        live.erase("tools");
        if (live.contains("run") && live.at("run").at("taskId").get<std::int64_t>() == runtime.taskId()) {
            for (const std::int64_t user : applied->users) {
                live["run"]["inputs"].push_back(user);
            }
        }
        auto successor = m_runs.createGeneration(tx, conversationId);
        if (!successor) {
            return std::unexpected(successor.error());
        }
        m_runs.handOver(live, runtime.taskId(), *successor);
        return {};
    }

    /** A tool task for call `callId`, owned by the generation. */
    Result<std::int64_t> createToolTask(Transaction& tx, ITaskRuntime& runtime, std::int64_t assistant, const std::string& callId) const {
        TaskOptions options;
        options.ownership = Json::object({{"kind", "task"}, {"taskId", runtime.taskId()}});
        return tx.createTask(m_tools.kind(), 1, Json::object({{"assistant", assistant}, {"callId", callId}}), Json::object({{"phase", "call"}}), options);
    }

    /** The calls `callIds` of the assistant entry, in the given order. */
    Result<std::vector<Json>> readCalls(ITaskRuntime& runtime, std::int64_t assistant, const Json& callIds) const {
        auto entry = runtime.entry(assistant, m_kinds.assistant());
        if (!entry) {
            return std::unexpected(entry.error());
        }
        std::vector<Json> calls;
        if (*entry && (*entry)->contains("model") && !(*entry)->at("model").empty()) {
            const Json& message = (*entry)->at("model")[0];
            for (const Json& id : callIds) {
                for (const Json& block : message.at("content")) {
                    if (block.value("type", std::string()) == "toolCall" && block.value("id", std::string()) == id.get<std::string>()) {
                        calls.push_back(block);
                        break;
                    }
                }
            }
        }
        return calls;
    }

    // ─── Thresholds and outcomes ────────────────────────────────────────────

    /**
     * Which threshold compaction preparation starts before its request: "blocking" above `contextWindow - reserveTokens`,
     * "background" above the background threshold, and only when range selection finds a cut; otherwise "". The caller
     * starts a background one only while no compaction is listed.
     */
    std::string thresholdCompaction(const Json& view, const std::vector<Json>& planned, std::int64_t contextWindow,
                                    const ConversationCompactionPolicy& policy) const {
        if (!policy.enabled || contextWindow <= 0) {
            return "";
        }
        Json extra = Json::array();
        for (const Json& entry : planned) {
            if (entry.contains("model")) {
                for (const Json& message : entry.at("model")) {
                    extra.push_back(message);
                }
            }
        }
        const std::int64_t tokens = m_compaction.estimateContext(view, extra);
        const std::int64_t blocking = contextWindow - policy.reserveTokens;
        const std::int64_t background = blocking - policy.backgroundTokens;
        std::string over;
        if (tokens > blocking) {
            over = "blocking";
        } else if (policy.backgroundTokens > 0 && tokens > background) {
            over = "background";
        }
        if (over.empty() || !m_compaction.selectCut(view, policy.keepRecentTokens)) {
            return "";
        }
        return over;
    }

    /** Settles the run's inputs `unanswered` with `model_error` and fails with `text`. */
    Result<void> failModelError(ITaskRuntime& runtime, const std::string& text) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, runtime.conversationId());
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto ended = m_live.endRun(tx, **live, runtime.taskId(),
                                           Json::object({{"status", "unanswered"}, {"reason", "model_error"}, {"detail", text}}));
                !ended) {
                return std::unexpected(ended.error());
            }
            return failed(text, "model_error");
        });
    }

    /** Settles the run's inputs `unanswered` with `no_model` and fails. */
    Result<void> failNoModel(ITaskRuntime& runtime, const std::string& message) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> State {
            auto live = liveOf(tx, runtime.conversationId());
            if (!live) {
                return std::unexpected(live.error());
            }
            if (auto ended = m_live.endRun(tx, **live, runtime.taskId(), Json::object({{"status", "unanswered"}, {"reason", "no_model"}})); !ended) {
                return std::unexpected(ended.error());
            }
            return failed(message, "no_model");
        });
    }

    // ─── State builders ─────────────────────────────────────────────────────

    std::optional<Json> waiting(const Json& checkpoint, const std::vector<std::int64_t>& on) const {
        return Json::object({{"status", "waiting"}, {"checkpoint", checkpoint}, {"on", Json(on)}, {"policy", "allSettled"}});
    }

    Json completed(const Json& result) const {
        return Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "completed"}, {"result", result}})}});
    }

    std::optional<Json> failed(const std::string& message, const std::string& reason) const {
        return Json::object({{"status", "terminal"},
                             {"outcome", Json::object({{"status", "failed"},
                                                       {"error", Json::object({{"message", message}, {"detail", Json::object({{"reason", reason}})}})}})}});
    }

    std::optional<std::int64_t> optionalId(const Json& object, const char* key) const {
        return object.contains(key) ? std::optional<std::int64_t>(object.at(key).get<std::int64_t>()) : std::nullopt;
    }

    DocAddressArgs ownerArgs(std::int64_t conversationId) const {
        DocAddressArgs args;
        args.owner = conversationId;
        return args;
    }

    Result<Json*> liveOf(Transaction& tx, std::int64_t conversationId) const {
        return tx.doc(m_documents.live(), ownerArgs(conversationId));
    }

    BuiltinDocuments m_documents;
    EntryKinds m_kinds;
    LiveEditor m_live;
    RunStarter m_runs;
    InboxBoundary m_boundary;
    PromptPlanner m_prompt;
    CompactionPlanner m_compaction;
    ModelRequests m_requests;
    ResponseClassifier m_classifier;
    AssistantEntries m_assistants;
    ToolResults m_results;
    ToolTaskDefinition m_tools;
    AgentConfigurator m_configurator;
};
