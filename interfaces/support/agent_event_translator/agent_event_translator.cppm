export module pi.support.agent_event_translator;

import std;
export import pi.support.builtin_documents;
export import pi.support.delta_differ;
export import pi.support.json_equality;
export import pi.types.json;

/**
 * Translates the committed publications of one conversation into agent events shaped like the coding agent's session
 * events (spec section 9.4, events.ts). Events are JSON objects with a `type` and the fields of the spec; `snapshot()` is
 * the starting point and every other event applies on top of it. One publication yields one batch, in the order of the spec:
 * tool starts, the in-flight message, tool updates, retry and deferred state; entries with their message events and tool
 * ends; tool ends without an entry; compaction ends, task failures, turn and run ends; submissions; document changes; and
 * last what began (compactions, run, turn).
 *
 * "Changed" means a different value: the views come from the diff-based mounts, which carry no object identity. Message and
 * tool output changes are derived from the two values (with a diff of the changed block or string), not from the view
 * operations, so they do not depend on how the mount's diff aligned the arrays.
 */
export class AgentEventTranslator {
public:
    /** The `snapshot` event of a view `{conversation, entries, docs}`. */
    Json snapshot(const Json& view) const {
        const Json live = liveOf(view);
        Json event = Json::object({{"type", "snapshot"}, {"entries", view.value("entries", Json::array())}});
        if (live.contains("run")) {
            event["run"] = Json::object({{"inputs", live.at("run").at("inputs")}});
        }
        if (live.contains("generation")) {
            event["generation"] = live.at("generation");
        }
        event["tools"] = live.value("tools", Json::array());
        event["compactions"] = live.value("compactions", Json::array());
        event["inbox"] = queued(doc(view, "pi.inbox"));
        event["agent"] = doc(view, "pi.agent").value_or(m_documents.agent().initial(Json()));
        event["usage"] = doc(view, "pi.usage").value_or(m_documents.usage().initial(Json()));
        return event;
    }

    /**
     * Every event one publication causes. `held` holds the generation tasks whose turn already ended at a `completing`
     * hold; it is updated, and starts with the ones found completing when the stream attached.
     */
    std::vector<Json> translate(std::int64_t conversationId, const Json& before, const Json& after, const Json& publication, std::set<std::int64_t>& held) const {
        std::vector<Json> entries;
        std::vector<std::pair<std::int64_t, Json>> tasks;
        std::vector<Json> submissions;
        for (const Json& change : publication.at("changes")) {
            const std::string type = change.value("type", std::string());
            if (type != "entry" && type != "task" && type != "submission") {
                continue;
            }
            const Json& value = change.at("value");
            if (value.value("conversationId", std::int64_t(0)) != conversationId) {
                continue;
            }
            if (type == "entry") {
                entries.push_back(value);
            } else if (type == "task") {
                remember(tasks, value);
            } else if (type == "submission") {
                submissions.push_back(value);
            }
        }
        if (entries.empty() && tasks.empty() && submissions.empty() && !differs(before, after)) {
            return {};
        }
        std::ranges::sort(submissions, [](const Json& a, const Json& b) { return a.at("id").get<std::int64_t>() < b.at("id").get<std::int64_t>(); });
        const Json liveBefore = liveOf(before);
        const Json liveAfter = liveOf(after);
        std::vector<Json> events;
        progress(events, liveBefore, liveAfter, tasks);
        std::vector<std::pair<Json, std::optional<std::int64_t>>> toolEnds = endedTools(liveBefore, liveAfter, entries);
        entryEvents(events, entries, toolEnds, liveBefore);
        for (const auto& end : toolEnds) {
            if (!end.second) {
                events.push_back(end.first);
            }
        }
        completions(events, liveBefore, liveAfter, tasks, held);
        const Json runBefore = liveBefore.value("run", Json());
        const Json run = liveAfter.value("run", Json());
        const bool runChanged = firstInput(run) != firstInput(runBefore);
        if (!runBefore.is_null() && runChanged) {
            events.push_back(Json::object({{"type", "run_end"}, {"inputs", runBefore.at("inputs")}}));
        }
        for (const Json& record : submissions) {
            events.push_back(Json::object({{"type", "submission"}, {"record", record}}));
        }
        documentEvents(events, before, after);
        compactionStarts(events, liveBefore, liveAfter);
        if (!run.is_null() && runChanged) {
            events.push_back(Json::object({{"type", "run_start"}, {"inputs", run.at("inputs")}}));
        }
        const std::optional<std::int64_t> taskBefore = runBefore.is_null() ? std::nullopt : std::optional<std::int64_t>(runBefore.at("taskId").get<std::int64_t>());
        if (!run.is_null() && std::optional<std::int64_t>(run.at("taskId").get<std::int64_t>()) != taskBefore) {
            const auto task = std::ranges::find_if(tasks, [&](const auto& entry) { return entry.first == run.at("taskId").get<std::int64_t>(); });
            if (task != tasks.end() && task->second.value("kind", std::string()) == "pi.generation") {
                events.push_back(Json::object({{"type", "turn_start"}}));
            }
        }
        return events;
    }

private:
    Json liveOf(const Json& view) const {
        return view.value("docs", Json::object()).value("pi.live", Json::object());
    }

    std::optional<Json> doc(const Json& view, const std::string& kind) const {
        const Json docs = view.value("docs", Json::object());
        if (!docs.contains(kind)) {
            return std::nullopt;
        }
        return docs.at(kind);
    }

    Json queued(const std::optional<Json>& inbox) const {
        Json items = Json::array();
        if (inbox) {
            for (const Json& item : inbox->value("items", Json::array())) {
                items.push_back(Json::object({{"id", item.at("id")}, {"mode", item.at("mode")}}));
            }
        }
        return items;
    }

    bool differs(const std::optional<Json>& a, const std::optional<Json>& b) const {
        if (a.has_value() != b.has_value()) {
            return true;
        }
        return a && !JsonEquality().equal(*a, *b);
    }

    bool differs(const Json& a, const Json& b) const {
        return !JsonEquality().equal(a, b);
    }

    void remember(std::vector<std::pair<std::int64_t, Json>>& tasks, const Json& record) const {
        const std::int64_t id = record.at("id").get<std::int64_t>();
        for (auto& entry : tasks) {
            if (entry.first == id) {
                entry.second = record;
                return;
            }
        }
        tasks.emplace_back(id, record);
    }

    std::optional<std::int64_t> firstInput(const Json& run) const {
        if (run.is_null() || !run.contains("inputs") || run.at("inputs").empty()) {
            return std::nullopt;
        }
        return run.at("inputs")[0].get<std::int64_t>();
    }

    std::map<std::string, Json> slotsByCall(const Json& live) const {
        std::map<std::string, Json> slots;
        for (const Json& slot : live.value("tools", Json::array())) {
            slots[slot.at("callId").get<std::string>()] = slot;
        }
        return slots;
    }

    // ─── Progress: tool starts, the in-flight message, tool updates, retry and deferred state ──────────

    void progress(std::vector<Json>& events, const Json& liveBefore, const Json& liveAfter, const std::vector<std::pair<std::int64_t, Json>>& tasks) const {
        const std::map<std::string, Json> slotsBefore = slotsByCall(liveBefore);
        const Json slots = liveAfter.value("tools", Json::array());
        for (const Json& slot : slots) {
            const auto previous = slotsBefore.find(slot.at("callId").get<std::string>());
            if (slot.value("status", std::string()) != "running" || (previous != slotsBefore.end() && previous->second.value("status", std::string()) == "running")) {
                continue;
            }
            events.push_back(Json::object({{"type", "tool_execution_start"}, {"toolCallId", slot.at("callId")}, {"toolName", slot.at("name")}, {"args", startArguments(slot, tasks)}}));
        }
        const Json generationBefore = liveBefore.value("generation", Json::object());
        const Json generation = liveAfter.value("generation", Json::object());
        const bool hadPartial = generationBefore.contains("message");
        if (generation.contains("message") && !hadPartial) {
            events.push_back(Json::object({{"type", "message_start"}, {"message", generation.at("message")}}));
        } else if (generation.contains("message") && differs(generation.at("message"), generationBefore.at("message"))) {
            events.push_back(Json::object({{"type", "message_update"}, {"usage", generation.at("message").value("usage", Json::object())}, {"changes", messageChanges(generationBefore.at("message"), generation.at("message"))}}));
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const Json& slot = slots[index];
            const auto previous = slotsBefore.find(slot.at("callId").get<std::string>());
            if (slot.value("status", std::string()) != "running" || previous == slotsBefore.end() || previous->second.value("status", std::string()) != "running") {
                continue;
            }
            if (auto update = toolUpdate(slot, previous->second)) {
                Json event = Json::object({{"type", "tool_execution_update"}, {"toolCallId", slot.at("callId")}, {"toolName", slot.at("name")}});
                for (const auto& field : update->items()) {
                    event[field.key()] = field.value();
                }
                events.push_back(std::move(event));
            }
        }
        if (generation.contains("retry") && !generationBefore.contains("retry")) {
            events.push_back(Json::object({{"type", "auto_retry_start"}, {"attempt", generation.at("attempt")}, {"at", generation.at("retry").at("at")}, {"errorMessage", generation.at("retry").at("error")}}));
        }
        if (generationBefore.contains("retry") && !generation.contains("retry")) {
            events.push_back(Json::object({{"type", "auto_retry_end"}, {"attempt", generationBefore.at("attempt")}}));
        }
        if (generation.contains("deferred")) {
            const Json pollAt = generation.at("deferred").at("pollAt");
            if (!generationBefore.contains("deferred") || differs(pollAt, generationBefore.at("deferred").at("pollAt"))) {
                events.push_back(Json::object({{"type", "deferred_poll"}, {"pollAt", pollAt}}));
            }
        }
    }

    Json startArguments(const Json& slot, const std::vector<std::pair<std::int64_t, Json>>& tasks) const {
        if (!slot.contains("taskId")) {
            return Json::object();
        }
        for (const auto& task : tasks) {
            if (task.first == slot.at("taskId").get<std::int64_t>()) {
                return task.second.value("state", Json::object()).value("checkpoint", Json::object()).value("arguments", Json::object());
            }
        }
        return Json::object();
    }

    // ─── Tool ends, entries ────────────────────────────────────────────────────────────────────────────

    /** Tools that end in this commit with the id of their result entry: a slot that becomes done, one created done, or one
     *  that vanishes unfinished because its run ended. A done slot that vanishes ended earlier. */
    std::vector<std::pair<Json, std::optional<std::int64_t>>> endedTools(const Json& liveBefore, const Json& liveAfter, const std::vector<Json>& entries) const {
        std::vector<std::pair<Json, std::optional<std::int64_t>>> ends;
        const std::map<std::string, Json> slotsBefore = slotsByCall(liveBefore);
        const std::map<std::string, Json> slotsAfter = slotsByCall(liveAfter);
        const auto end = [&](const std::string& callId, const Json& name, std::optional<std::int64_t> entryId) {
            Json event = Json::object({{"type", "tool_execution_end"}, {"toolCallId", callId}, {"toolName", name}});
            if (entryId) {
                const auto entry = std::ranges::find_if(entries, [&](const Json& candidate) { return candidate.at("id").get<std::int64_t>() == *entryId; });
                if (entry != entries.end()) {
                    event["entry"] = *entry;
                } else {
                    entryId.reset();
                }
            }
            ends.emplace_back(std::move(event), entryId);
        };
        for (const Json& previous : liveBefore.value("tools", Json::array())) {
            if (previous.value("status", std::string()) == "done") {
                continue;
            }
            const std::string callId = previous.at("callId").get<std::string>();
            const auto slot = slotsAfter.find(callId);
            if (slot != slotsAfter.end() && slot->second.value("status", std::string()) == "done") {
                end(callId, previous.at("name"), slot->second.contains("entry") ? std::optional<std::int64_t>(slot->second.at("entry").get<std::int64_t>()) : std::nullopt);
            } else if (slot == slotsAfter.end()) {
                end(callId, previous.at("name"), resultEntryId(entries, callId));
            }
        }
        for (const Json& slot : liveAfter.value("tools", Json::array())) {
            const std::string callId = slot.at("callId").get<std::string>();
            if (slot.value("status", std::string()) == "done" && !slotsBefore.contains(callId)) {
                end(callId, slot.at("name"), slot.contains("entry") ? std::optional<std::int64_t>(slot.at("entry").get<std::int64_t>()) : std::nullopt);
            }
        }
        return ends;
    }

    std::optional<std::int64_t> resultEntryId(const std::vector<Json>& entries, const std::string& callId) const {
        for (const Json& entry : entries) {
            const Json message = firstMessage(entry);
            if (message.value("role", std::string()) == "toolResult" && message.value("toolCallId", std::string()) == callId) {
                return entry.at("id").get<std::int64_t>();
            }
        }
        return std::nullopt;
    }

    Json firstMessage(const Json& entry) const {
        if (!entry.contains("model") || !entry.at("model").is_array() || entry.at("model").empty()) {
            return Json();
        }
        return entry.at("model")[0];
    }

    /** Entries in append order; a tool's end directly precedes its result's message, as in the coding agent. */
    void entryEvents(std::vector<Json>& events, const std::vector<Json>& entries, const std::vector<std::pair<Json, std::optional<std::int64_t>>>& toolEnds, const Json& liveBefore) const {
        const bool partialBefore = liveBefore.value("generation", Json::object()).contains("message");
        bool assistantAppended = false;
        for (const Json& entry : entries) {
            const std::int64_t id = entry.at("id").get<std::int64_t>();
            for (const auto& end : toolEnds) {
                if (end.second == id) {
                    events.push_back(end.first);
                }
            }
            const Json message = firstMessage(entry);
            if (message.is_null()) {
                events.push_back(Json::object({{"type", "entry_appended"}, {"entry", entry}}));
                continue;
            }
            const bool assistant = message.value("role", std::string()) == "assistant";
            // A streamed answer already started with its first partial.
            const bool streamed = assistant && partialBefore && !assistantAppended;
            assistantAppended = assistantAppended || assistant;
            if (!streamed) {
                events.push_back(Json::object({{"type", "message_start"}, {"message", message}}));
            }
            events.push_back(Json::object({{"type", "message_end"}, {"entry", entry}}));
        }
    }

    // ─── Compactions, task failures, turn ends ─────────────────────────────────────────────────────────

    void completions(std::vector<Json>& events, const Json& liveBefore, const Json& liveAfter, const std::vector<std::pair<std::int64_t, Json>>& tasks, std::set<std::int64_t>& held) const {
        const Json compactionsBefore = liveBefore.value("compactions", Json::array());
        const Json compactions = liveAfter.value("compactions", Json::array());
        for (const Json& status : compactionsBefore) {
            if (!hasTask(compactions, status.at("taskId"))) {
                events.push_back(Json::object({{"type", "compaction_end"}, {"taskId", status.at("taskId")}, {"reason", status.at("reason")}}));
            }
        }
        // A generation's turn ends when its outcome is committed: at a `completing` hold or at terminal, whichever comes
        // first, so a successor created at the hold starts after it.
        bool turnEnded = false;
        for (const auto& entry : tasks) {
            const Json& task = entry.second;
            const std::string status = task.at("state").at("status").get<std::string>();
            const bool generation = task.value("kind", std::string()) == "pi.generation";
            if (generation && status == "completing" && !held.contains(entry.first)) {
                held.insert(entry.first);
                turnEnded = true;
            }
            if (status != "terminal") {
                continue;
            }
            if (generation && held.erase(entry.first) == 0) {
                turnEnded = true;
            }
            const Json outcome = task.at("state").at("outcome");
            const std::string result = outcome.value("status", std::string());
            if (result == "faulted" || result == "orphaned") {
                const std::string message = result == "faulted" ? outcome.value("error", Json::object()).value("message", std::string()) : outcome.value("reason", std::string());
                events.push_back(Json::object({{"type", "task_failed"}, {"taskId", entry.first}, {"kind", task.at("kind")}, {"message", message}}));
            }
        }
        if (turnEnded) {
            events.push_back(Json::object({{"type", "turn_end"}}));
        }
    }

    bool hasTask(const Json& statuses, const Json& taskId) const {
        return std::ranges::any_of(statuses, [&](const Json& status) { return status.at("taskId") == taskId; });
    }

    void compactionStarts(std::vector<Json>& events, const Json& liveBefore, const Json& liveAfter) const {
        const Json before = liveBefore.value("compactions", Json::array());
        for (const Json& status : liveAfter.value("compactions", Json::array())) {
            if (!hasTask(before, status.at("taskId"))) {
                events.push_back(Json::object({{"type", "compaction_start"}, {"taskId", status.at("taskId")}, {"reason", status.at("reason")}, {"blocking", status.value("blocking", false)}}));
            }
        }
    }

    /** A retired document reads as its initial value, as in a snapshot. */
    void documentEvents(std::vector<Json>& events, const Json& before, const Json& after) const {
        const std::optional<Json> inboxBefore = doc(before, "pi.inbox");
        const std::optional<Json> inbox = doc(after, "pi.inbox");
        if (differs(inbox, inboxBefore)) {
            events.push_back(Json::object({{"type", "inbox_update"}, {"items", queued(inbox)}}));
        }
        const std::optional<Json> agent = doc(after, "pi.agent");
        if (differs(agent, doc(before, "pi.agent"))) {
            events.push_back(Json::object({{"type", "agent_changed"}, {"agent", agent.value_or(m_documents.agent().initial(Json()))}}));
        }
        const std::optional<Json> usage = doc(after, "pi.usage");
        if (differs(usage, doc(before, "pi.usage"))) {
            events.push_back(Json::object({{"type", "usage_changed"}, {"usage", usage.value_or(m_documents.usage().initial(Json()))}}));
        }
    }

    // ─── Message changes and tool updates, from the values ─────────────────────────────────────────────

    /**
     * The changes that turn the previous partial message into this one: blocks appended to `content` start, an append to a
     * block's `text` or `thinking` is a delta, an append to a string inside a tool call's `arguments` is a `toolcall_delta`
     * (path relative to `arguments`), any other change inside a block sends that `block`, and anything else, including a
     * shrinking `content` or a changed field other than `usage`, sends the whole `message`. A change of only `usage` has no
     * changes.
     */
    Json messageChanges(const Json& previous, const Json& message) const {
        const Json whole = Json::array({Json::object({{"type", "message"}, {"message", message}})});
        for (const auto& field : message.items()) {
            if (field.key() != "content" && field.key() != "usage" && (!previous.contains(field.key()) || differs(previous.at(field.key()), field.value()))) {
                return whole;
            }
        }
        for (const auto& field : previous.items()) {
            if (field.key() != "content" && field.key() != "usage" && !message.contains(field.key())) {
                return whole;
            }
        }
        const Json before = previous.value("content", Json::array());
        const Json after = message.value("content", Json::array());
        if (after.size() < before.size()) {
            return whole;
        }
        Json changes = Json::array();
        for (std::size_t index = 0; index < before.size(); ++index) {
            if (differs(before[index], after[index])) {
                blockChanges(changes, static_cast<std::int64_t>(index), before[index], after[index]);
            }
        }
        for (std::size_t index = before.size(); index < after.size(); ++index) {
            const std::string type = after[index].value("type", std::string());
            const std::string start = type == "text" ? "text_start" : type == "thinking" ? "thinking_start" : "toolcall_start";
            changes.push_back(Json::object({{"type", start}, {"contentIndex", static_cast<std::int64_t>(index)}, {"block", after[index]}}));
        }
        return changes;
    }

    /** Deltas when the block only grew in text, thinking or tool call arguments; else the whole block. */
    void blockChanges(Json& changes, std::int64_t contentIndex, const Json& before, const Json& after) const {
        const Json ops = DeltaDiffer().diff(before, after);
        Json deltas = Json::array();
        for (const Json& op : ops) {
            const std::string kind = op.at(0).get<std::string>();
            if (kind != "a") {
                deltas = Json();
                break;
            }
            const Json& path = op.at(1);
            const Json field = path.empty() ? Json() : path[0];
            if (path.size() == 1 && field == "text") {
                deltas.push_back(Json::object({{"type", "text_delta"}, {"contentIndex", contentIndex}, {"delta", op.at(2)}}));
            } else if (path.size() == 1 && field == "thinking") {
                deltas.push_back(Json::object({{"type", "thinking_delta"}, {"contentIndex", contentIndex}, {"delta", op.at(2)}}));
            } else if (field == "arguments" && path.size() > 1) {
                deltas.push_back(Json::object({{"type", "toolcall_delta"}, {"contentIndex", contentIndex}, {"path", Json(std::vector<Json>(path.begin() + 1, path.end()))}, {"delta", op.at(2)}}));
            } else {
                deltas = Json();
                break;
            }
        }
        if (deltas.is_array() && !deltas.empty()) {
            for (const Json& delta : deltas) {
                changes.push_back(delta);
            }
            return;
        }
        changes.push_back(Json::object({{"type", "block"}, {"contentIndex", contentIndex}, {"block", after}}));
    }

    /** Output, details and diagnostics changes of a running slot; nothing when none changed. */
    std::optional<Json> toolUpdate(const Json& slot, const Json& previous) const {
        Json update = Json::object();
        const std::string before = previous.value("output", std::string());
        const std::string after = slot.value("output", std::string());
        if (before != after) {
            // A front trim and then an append of the retained window, or a replacement.
            const Json ops = DeltaDiffer().diff(Json::object({{"o", before}}), Json::object({{"o", after}}));
            std::int64_t trimStart = 0;
            std::string append;
            bool set = false;
            for (const Json& op : ops) {
                const std::string kind = op.at(0).get<std::string>();
                if (kind == "t") {
                    trimStart += op.at(2).get<std::int64_t>();
                } else if (kind == "a") {
                    append += op.at(2).get<std::string>();
                } else {
                    set = true;
                }
            }
            if (set) {
                update["output"] = Json::object({{"set", after}});
            } else {
                Json change = Json::object();
                if (trimStart > 0) {
                    change["trimStart"] = trimStart;
                }
                if (!append.empty()) {
                    change["append"] = append;
                }
                update["output"] = change;
            }
        }
        // A safe replay clears a running slot's progress: removed details send `null`, removed diagnostics `[]`.
        if (slot.contains("details") != previous.contains("details") || differs(slot.value("details", Json()), previous.value("details", Json()))) {
            update["details"] = slot.contains("details") ? slot.at("details") : Json(nullptr);
        }
        if (slot.contains("diagnostics") != previous.contains("diagnostics") || differs(slot.value("diagnostics", Json()), previous.value("diagnostics", Json()))) {
            update["diagnostics"] = slot.contains("diagnostics") ? slot.at("diagnostics") : Json::array();
        }
        if (update.empty()) {
            return std::nullopt;
        }
        return update;
    }

    BuiltinDocuments m_documents;
};
