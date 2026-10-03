module;

#include <cstdint>

export module pi.support.compaction_task_definition;

import std;
export import pi.durable.task_definition;
export import pi.support.builtin_documents;
export import pi.support.compaction_planner;
export import pi.support.entry_kinds;
export import pi.support.live_editor;
export import pi.support.model_requests;
export import pi.support.response_classifier;
export import pi.support.submission_admission;
export import pi.support.usage_ledger;

/**
 * The built-in compaction task (`pi.compaction`, spec §8.7): selects an old prefix of the model context, summarizes it,
 * and places a summary entry whose `head` is the first kept entry. A compaction the generation owns blocks it and
 * appends directly; a conversation-owned one places its summary through a write submission. Port of CompactionTask in
 * packages/durable/src/harness/compaction.ts.
 *
 * Input `{reason, instructions?}`. Checkpoints `{phase: "select"}`, `{phase: "summarize", attempt, model,
 * thinkingLevel, streamOptions, maxTokens, tail, firstKept}` and the same with `phase: "retry", until`. Result `{}`,
 * `{entryId}` or `{submissionId}`.
 */
export class CompactionTaskDefinition {
public:
    std::shared_ptr<TaskDefinition> build() const {
        auto definition = std::make_shared<TaskDefinition>();
        definition->name = m_planner.taskKind();
        definition->version = 1;
        definition->initial = [](const Json&) { return Json::object({{"phase", "select"}}); };
        definition->phases["select"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.select(task, runtime); };
        definition->phases["summarize"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.summarize(task, runtime); };
        definition->phases["retry"] = [self = *this](const Json& task, ITaskRuntime& runtime) { return self.retry(task, runtime); };
        definition->abort = [self = *this](const Json&, ITaskRuntime& runtime) { return self.abort(runtime); };
        return definition;
    }

private:
    // ─── Phases ─────────────────────────────────────────────────────────────

    Result<void> select(const Json& task, ITaskRuntime& runtime) const {
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
        auto view = runtime.context(runtime.conversationId());
        if (!view) {
            return std::unexpected(view.error());
        }
        const auto cut = m_planner.selectCut(*view, settings.compaction.keepRecentTokens);
        if (!cut) {
            return complete(runtime);
        }
        const std::int64_t firstKept = view->at("entries")[*cut].at("id").get<std::int64_t>();
        Json compaction = Json::object({{"reason", task.at("input").at("reason")}});
        Json before = Json::array();
        for (std::size_t i = 0; i < *cut; ++i) {
            before.push_back(view->at("entries")[i]);
        }
        compaction["entries"] = before;
        compaction["messages"] = m_planner.summarizedMessages(*view, *cut);
        compaction["firstKept"] = firstKept;
        if (task.at("input").contains("instructions")) {
            compaction["instructions"] = task.at("input").at("instructions");
        }
        std::optional<Json> decision;
        auto hooked = runtime.eachHook("beforeCompact", [&](const HookHandler& hook) -> Result<void> {
            if (decision) {
                return {};
            }
            auto decided = hook(compaction, runtime);
            if (!decided) {
                return std::unexpected(decided.error());
            }
            decision = *decided;
            return {};
        });
        if (!hooked) {
            return hooked;
        }
        if (decision && decision->contains("decline")) {
            return complete(runtime);
        }
        if (decision && decision->contains("summary")) {
            return place(runtime, firstKept, decision->at("summary").get<std::string>());
        }
        std::int64_t maxTokens = settings.compaction.reserveTokens * 4 / 5;
        if (model->maxTokens > 0) {
            maxTokens = std::min(maxTokens, model->maxTokens);
        }
        std::int64_t tail = firstKept;
        for (const Json& entry : view->at("entries")) {
            tail = std::max(tail, entry.at("id").get<std::int64_t>());
        }
        Json request = Json::object({{"phase", "summarize"},
                                     {"attempt", 1},
                                     {"model", *snapshot->model},
                                     {"thinkingLevel", snapshot->thinkingLevel},
                                     {"streamOptions", settings.stream},
                                     {"maxTokens", maxTokens},
                                     {"tail", tail},
                                     {"firstKept", firstKept}});
        return runtime.commit([&](Transaction&, const Json&) -> Result<std::optional<Json>> {
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", request}}));
        });
    }

    Result<void> summarize(const Json& task, ITaskRuntime& runtime) const {
        const Json& request = task.at("state").at("checkpoint");
        const std::int64_t attempt = request.at("attempt").get<std::int64_t>();
        const std::int64_t firstKept = request.at("firstKept").get<std::int64_t>();
        auto model = m_requests.find(runtime.models(), request.at("model"));
        if (!model) {
            return failNoModel(runtime, model.error().message);
        }
        // The context at `tail` is immutable, so this is the range `select` chose.
        auto view = runtime.context(runtime.conversationId(), request.at("tail").get<std::int64_t>());
        if (!view) {
            return std::unexpected(view.error());
        }
        std::size_t cut = 0;
        for (std::size_t i = 0; i < view->at("entries").size(); ++i) {
            if (view->at("entries")[i].at("id").get<std::int64_t>() == firstKept) {
                cut = i;
                break;
            }
        }
        const std::int64_t now = runtime.now();
        std::optional<std::string> instructions;
        if (task.at("input").contains("instructions")) {
            instructions = task.at("input").at("instructions").get<std::string>();
        }
        const Json messages = Json::array(
            {Json::object({{"role", "system"}, {"content", m_planner.systemPrompt()}, {"timestamp", now}}),
             Json::object({{"role", "user"},
                           {"content", Json::array({Json::object({{"type", "text"}, {"text", m_planner.summaryPrompt(m_planner.summarizedMessages(*view, cut), instructions)}})})},
                           {"timestamp", now}})});
        Json stream = request.at("streamOptions");
        stream.erase("deferred");
        stream["cacheRetention"] = "none";
        StreamOptions options = m_requests.options(stream, request.value("thinkingLevel", std::string("off")), runtime.signal());
        options.maxTokens = request.at("maxTokens").get<std::int64_t>();
        auto message = m_requests.complete(*runtime.models(), *model, messages, options);
        if (!message) {
            return std::unexpected(message.error());
        }
        // An abort mark or close: the abort invocation or the reopened task handles the committed state.
        if (runtime.signal().aborted()) {
            return std::unexpected(Error{"aborted", "The operation was aborted"});
        }
        const std::optional<std::string> summary = m_planner.summaryText(*message);
        const ConversationRetryPolicy policy = runtime.settings().retry;
        const bool retry = message->value("stopReason", std::string()) == "error" && m_classifier.retryable(*message) &&
                           policy.enabled && attempt <= policy.maxRetries;
        const std::int64_t until = retry ? runtime.now() + m_classifier.retryDelayMs(policy, attempt) : 0;
        return runtime.commit([&](Transaction& tx, const Json& current) -> Result<std::optional<Json>> {
            const std::string usageKey = message->value("provider", std::string()) + "/" + message->value("model", std::string());
            if (message->contains("usage")) {
                if (auto recorded = m_ledger.record(tx, runtime.conversationId(), "models", usageKey, message->at("usage")); !recorded) {
                    return std::unexpected(recorded.error());
                }
            }
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (summary) {
                return placeSummary(tx, runtime, current, **live, firstKept, *summary);
            }
            if (retry) {
                if (Json* status = m_live.compactionStatus(**live, runtime.taskId())) {
                    (*status)["retry"] = Json::object({{"at", until}, {"error", message->value("errorMessage", std::string())}});
                }
                Json next = request;
                next["phase"] = "retry";
                next["until"] = until;
                return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", next}}));
            }
            m_live.removeCompactionStatus(**live, runtime.taskId());
            const std::string text = m_planner.summaryFailure(*message);
            return std::optional<Json>(Json::object({{"status", "terminal"},
                                                     {"outcome", Json::object({{"status", "failed"},
                                                                               {"error", Json::object({{"message", text}, {"detail", Json::object({{"reason", "model_error"}})}})}})}}));
        });
    }

    Result<void> retry(const Json& task, ITaskRuntime& runtime) const {
        const Json& request = task.at("state").at("checkpoint");
        if (auto slept = runtime.sleep(request.at("until").get<std::int64_t>()); !slept) {
            return slept;
        }
        const std::int64_t attempt = request.at("attempt").get<std::int64_t>() + 1;
        return runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            if (Json* status = m_live.compactionStatus(**live, runtime.taskId())) {
                (*status)["attempt"] = attempt;
                status->erase("retry");
            }
            Json next = request;
            next["phase"] = "summarize";
            next["attempt"] = attempt;
            next.erase("until");
            return std::optional<Json>(Json::object({{"status", "running"}, {"checkpoint", next}}));
        });
    }

    Result<void> abort(ITaskRuntime& runtime) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            m_live.removeCompactionStatus(**live, runtime.taskId());
            return std::optional<Json>(Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "aborted"}})}}));
        });
    }

    // ─── Outcomes ───────────────────────────────────────────────────────────

    Result<Json*> liveOf(Transaction& tx, ITaskRuntime& runtime) const {
        DocAddressArgs address;
        address.owner = runtime.conversationId();
        return tx.doc(m_documents.live(), address);
    }

    /** Places a summary supplied by a hook in its own commit. */
    Result<void> place(ITaskRuntime& runtime, std::int64_t firstKept, const std::string& summary) const {
        return runtime.commit([&](Transaction& tx, const Json& current) -> Result<std::optional<Json>> {
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            return placeSummary(tx, runtime, current, **live, firstKept, summary);
        });
    }

    /**
     * Places the summary entry and completes. A blocking compaction, owned by its generation, appends it: the
     * generation holds the run and waits. A conversation-owned one admits it as a write submission, placed at once when
     * idle, otherwise at the next boundary, or settled `stale`. Nothing else may append to a busy conversation, so every
     * non-blocking summary goes through admission.
     */
    Result<std::optional<Json>> placeSummary(Transaction& tx, ITaskRuntime& runtime, const Json& current, Json& live,
                                             std::int64_t firstKept, const std::string& summary) const {
        m_live.removeCompactionStatus(live, runtime.taskId());
        const std::string text = m_planner.summaryPrefix() + summary + m_planner.summarySuffix();
        const Json message = Json::object({{"role", "user"},
                                           {"content", Json::array({Json::object({{"type", "text"}, {"text", text}})})},
                                           {"timestamp", runtime.now()}});
        const Json entry = Json::object({{"kind", m_kinds.compaction()},
                                         {"head", firstKept},
                                         {"model", Json::array({message})},
                                         {"data", Json::object({{"reason", current.at("input").at("reason")}})}});
        Json result = Json::object();
        if (!current.contains("owner")) {
            SubmissionDraft draft;
            draft.type = "write";
            draft.requestId = "compaction:" + std::to_string(runtime.taskId());
            draft.entry = entry;
            const ResolvedSettings settings = runtime.settings();
            auto admitted = m_admission.admit(tx, runtime.conversationId(), draft, runtime.now(), settings.steeringMode, settings.followUpMode);
            if (!admitted) {
                return std::unexpected(admitted.error());
            }
            result["submissionId"] = *admitted;
        } else {
            auto appended = tx.appendEntry(runtime.conversationId(), entry);
            if (!appended) {
                return std::unexpected(appended.error());
            }
            result["entryId"] = appended->at("id");
        }
        return std::optional<Json>(Json::object({{"status", "terminal"}, {"outcome", Json::object({{"status", "completed"}, {"result", result}})}}));
    }

    /** Removes the status and completes without a summary. */
    Result<void> complete(ITaskRuntime& runtime) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            m_live.removeCompactionStatus(**live, runtime.taskId());
            return std::optional<Json>(Json::object({{"status", "terminal"},
                                                     {"outcome", Json::object({{"status", "completed"}, {"result", Json::object()}})}}));
        });
    }

    Result<void> failNoModel(ITaskRuntime& runtime, const std::string& message) const {
        return runtime.commit([&](Transaction& tx, const Json&) -> Result<std::optional<Json>> {
            auto live = liveOf(tx, runtime);
            if (!live) {
                return std::unexpected(live.error());
            }
            m_live.removeCompactionStatus(**live, runtime.taskId());
            return std::optional<Json>(Json::object({{"status", "terminal"},
                                                     {"outcome", Json::object({{"status", "failed"},
                                                                               {"error", Json::object({{"message", message}, {"detail", Json::object({{"reason", "no_model"}})}})}})}}));
        });
    }

    BuiltinDocuments m_documents;
    EntryKinds m_kinds;
    LiveEditor m_live;
    CompactionPlanner m_planner;
    ModelRequests m_requests;
    ResponseClassifier m_classifier;
    SubmissionAdmission m_admission;
    UsageLedger m_ledger;
};
