export module pi.support.plugin_session_events;

import std;
export import pi.plugin.i_hook_bus;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.types.agent_start_outcome;
export import pi.types.before_compact_outcome;
export import pi.types.bash_result;
export import pi.types.before_tree_outcome;
export import pi.types.compaction_preparation;
export import pi.types.image_content;
export import pi.types.input_outcome;
export import pi.types.session_entry;
export import pi.types.tree_preparation;

/**
 * The plugin events around a session that are not part of the agent loop's hook points. Handlers run in subscription
 * order and see the changes of earlier ones; a handler that fails or answers null has no opinion. Events (payload ->
 * result):
 *  - `input` {text, images?, source, streamingBehavior?} -> {action: "continue"} | {action: "transform", text, images?}
 *    | {action: "handled"} ("handled" ends the chain and swallows the input);
 *  - `before_agent_start` {prompt, images?, systemPrompt} -> {message?: {customType, content?, display?, details?},
 *    systemPrompt?} (the last system prompt wins; the later handlers see it);
 *  - `before_provider_request` {payload} -> any JSON replacing the payload, null keeps it;
 *  - `session_before_compact` {preparation, branchEntries, customInstructions?, reason, willRetry} -> {cancel?,
 *    compaction?: {summary, firstKeptEntryId, tokensBefore, details?, usage?}} (a cancel ends the chain);
 *  - `session_compact` {compactionEntry, fromExtension, reason, willRetry} and `session_compact_failed` {reason,
 *    errorMessage?, aborted, willRetry, fromExtension}: observations;
 *  - `session_before_tree` {preparation} -> {cancel?, summary?: {summary, details?, usage?}, customInstructions?,
 *    replaceInstructions?, label?} (a cancel ends the chain) and `session_tree` {newLeafId, oldLeafId, summaryEntry?,
 *    fromExtension?}: observation;
 *  - `session_before_switch` {reason: "new"|"resume", targetSessionFile?} and `session_before_fork` {entryId, position:
 *    "before"|"at"} -> {cancel?}: a cancel keeps the current session;
 *  - `project_trust` {cwd} -> {trusted: "yes"|"no"|"undecided", remember?}: the first "yes" or "no" decides whether the project
 *    is trusted (and is stored when `remember`); "undecided" or no answer leaves it to the stored decision and the settings;
 *  - `resources_discover` {cwd, reason: "startup"|"reload"} -> {skillPaths?, promptPaths?}: more skill and prompt template paths
 *    (every handler's answer is used);
 *  - `user_bash` {command, excludeFromContext, cwd} -> {result?: {output, exitCode?, cancelled?, truncated?, fullOutputPath?}}: the
 *    first handler with a result replaces running the command (TypeScript's custom `operations` cannot cross the C ABI);
 *  - `after_provider_response` {status, headers}: the provider answered (2xx) and the stream starts; observation;
 *  - `provider_stream_event` {provider, api, model, data}: one parsed JSON event of a server-sent-events response before pi
 *    interprets it; observation (not delivered for AWS event streams and WebSockets);
 *  - `model_select` {model, previousModel?, source: "set"|"cycle"} and `thinking_level_select` {level, previousLevel}: observations
 *    of changes that actually changed something.
 * Without subscribers every method answers "nothing changed".
 */
export class PluginSessionEvents {
public:
    explicit PluginSessionEvents(IHookBus& bus)
        : m_bus(bus) {}

    bool hasHandlers(const std::string& event) const {
        return m_bus.hasHandlers(event);
    }

    InputOutcome input(const std::string& text, const std::vector<ImageContent>& images, const std::string& source, const std::string& streamingBehavior) {
        InputOutcome outcome;
        outcome.text = text;
        outcome.images = images;
        if (!m_bus.hasHandlers("input")) {
            return outcome;
        }
        Json payload = Json::object({{"text", text}, {"source", source}});
        if (!images.empty()) {
            payload["images"] = imagesJson(images);
        }
        if (!streamingBehavior.empty()) {
            payload["streamingBehavior"] = streamingBehavior;
        }
        bool handled = false;
        const HookOutcome emitted = m_bus.emit("input", payload, [&handled](Json& current, const Json& result) {
            if (handled || !result.is_object()) {
                return;
            }
            const std::string action = result.value("action", "");
            if (action == "handled") {
                handled = true;
            } else if (action == "transform" && result.contains("text") && result["text"].is_string()) {
                current["text"] = result["text"];
                if (result.contains("images") && result["images"].is_array()) {
                    current["images"] = result["images"];
                }
            }
        });
        outcome.handled = handled;
        if (emitted.payload.is_object() && emitted.payload.contains("text") && emitted.payload["text"].is_string()) {
            outcome.text = emitted.payload["text"].get<std::string>();
            outcome.images = emitted.payload.contains("images") ? imagesFrom(emitted.payload["images"], images) : images;
        }
        return outcome;
    }

    AgentStartOutcome beforeAgentStart(const std::string& prompt, const std::vector<ImageContent>& images, const std::string& systemPrompt) {
        AgentStartOutcome outcome;
        if (!m_bus.hasHandlers("before_agent_start")) {
            return outcome;
        }
        Json payload = Json::object({{"prompt", prompt}, {"systemPrompt", systemPrompt}});
        if (!images.empty()) {
            payload["images"] = imagesJson(images);
        }
        const HookOutcome emitted = m_bus.emit("before_agent_start", payload, [this, &outcome](Json& current, const Json& result) {
            if (!result.is_object()) {
                return;
            }
            if (result.contains("message") && result["message"].is_object()) {
                outcome.messages.push_back(normalizedMessage(result["message"]));
            }
            if (result.contains("systemPrompt") && result["systemPrompt"].is_string()) {
                outcome.systemPrompt = result["systemPrompt"].get<std::string>();
                current["systemPrompt"] = result["systemPrompt"];
            }
        });
        (void)emitted;
        return outcome;
    }

    /** The payload after every handler; nullopt when nobody replaced it. */
    std::optional<Json> beforeProviderRequest(const Json& payload) {
        if (!m_bus.hasHandlers("before_provider_request")) {
            return std::nullopt;
        }
        bool replaced = false;
        const HookOutcome emitted = m_bus.emit("before_provider_request", Json::object({{"payload", payload}}),
                                               [&replaced](Json& current, const Json& result) {
                                                   if (!result.is_null()) {
                                                       current["payload"] = result;
                                                       replaced = true;
                                                   }
                                               });
        if (!replaced) {
            return std::nullopt;
        }
        return emitted.payload["payload"];
    }

    /**
     * The request headers (name to value) after every `before_provider_headers` handler. A handler answers an object of the
     * headers to set (`null` removes one); TypeScript handlers mutate the headers in place, which a C ABI cannot.
     */
    Json beforeProviderHeaders(const Json& headers) {
        if (!m_bus.hasHandlers("before_provider_headers")) {
            return headers;
        }
        const HookOutcome emitted = m_bus.emit("before_provider_headers", Json::object({{"headers", headers}}),
                                               [](Json& current, const Json& result) {
                                                   if (!result.is_object()) {
                                                       return;
                                                   }
                                                   for (const auto& entry : result.items()) {
                                                       if (entry.value().is_null()) {
                                                           current["headers"].erase(entry.key());
                                                       } else if (entry.value().is_string()) {
                                                           current["headers"][entry.key()] = entry.value();
                                                       }
                                                   }
                                               });
        return emitted.payload["headers"];
    }

    /**
     * What a `cache_warming_decision` handler wants done with the refresh pi decided on (`warm` or `stop`): the payload carries
     * the prices and pi's action, a handler answers `{"action": "warm" | "stop"}`, the last answer wins; `action` is returned
     * when nobody answers.
     */
    std::string cacheWarmingDecision(double warmCost, double missCost, double continuationProbability, const std::string& action) {
        if (!m_bus.hasHandlers("cache_warming_decision")) {
            return action;
        }
        const HookOutcome emitted = m_bus.emit("cache_warming_decision",
                                               Json::object({{"warmCost", warmCost}, {"missCost", missCost}, {"continuationProbability", continuationProbability}, {"action", action}}),
                                               [](Json& current, const Json& result) {
                                                   if (result.is_object() && result.contains("action") && result["action"].is_string()) {
                                                       const std::string next = result["action"].get<std::string>();
                                                       if (next == "warm" || next == "stop") {
                                                           current["action"] = next;
                                                       }
                                                   }
                                               });
        return emitted.payload.value("action", action);
    }

    BeforeCompactOutcome beforeCompact(const CompactionPreparation& preparation, const std::vector<SessionEntry>& branch, const std::optional<std::string>& customInstructions, const std::string& reason, bool willRetry) {
        BeforeCompactOutcome outcome;
        if (!m_bus.hasHandlers("session_before_compact")) {
            return outcome;
        }
        Json payload = Json::object({{"preparation", preparationJson(preparation)},
                                     {"branchEntries", entriesJson(branch)},
                                     {"reason", reason},
                                     {"willRetry", willRetry}});
        if (customInstructions) {
            payload["customInstructions"] = *customInstructions;
        }
        const HookOutcome emitted = m_bus.emit("session_before_compact", payload);
        for (const Json& result : emitted.results) {
            if (!result.is_object()) {
                continue;
            }
            if (result.value("cancel", false)) {
                outcome.cancel = true;
                outcome.compaction.reset();
                return outcome;
            }
            if (auto compaction = compactionFrom(result)) {
                outcome.compaction = std::move(compaction);
            }
        }
        return outcome;
    }

    void compacted(const SessionEntry& entry, bool fromExtension, const std::string& reason, bool willRetry) {
        observe("session_compact", Json::object({{"compactionEntry", entry.body}, {"fromExtension", fromExtension}, {"reason", reason}, {"willRetry", willRetry}}));
    }

    void compactFailed(const std::string& reason, const std::optional<std::string>& errorMessage, bool aborted, bool willRetry, bool fromExtension) {
        Json payload = Json::object({{"reason", reason}, {"aborted", aborted}, {"willRetry", willRetry}, {"fromExtension", fromExtension}});
        if (errorMessage) {
            payload["errorMessage"] = *errorMessage;
        }
        observe("session_compact_failed", payload);
    }

    BeforeTreeOutcome beforeTree(const TreePreparation& preparation) {
        BeforeTreeOutcome outcome;
        if (!m_bus.hasHandlers("session_before_tree")) {
            return outcome;
        }
        Json prepared = Json::object({{"targetId", preparation.targetId},
                                      {"oldLeafId", optionalString(preparation.oldLeafId)},
                                      {"commonAncestorId", optionalString(preparation.commonAncestorId)},
                                      {"entriesToSummarize", entriesJson(preparation.entriesToSummarize)},
                                      {"userWantsSummary", preparation.userWantsSummary},
                                      {"replaceInstructions", preparation.replaceInstructions}});
        if (preparation.customInstructions) {
            prepared["customInstructions"] = *preparation.customInstructions;
        }
        if (preparation.label) {
            prepared["label"] = *preparation.label;
        }
        const HookOutcome emitted = m_bus.emit("session_before_tree", Json::object({{"preparation", prepared}}));
        for (const Json& result : emitted.results) {
            if (!result.is_object()) {
                continue;
            }
            if (result.value("cancel", false)) {
                outcome = BeforeTreeOutcome();
                outcome.cancel = true;
                return outcome;
            }
            mergeTree(result, outcome);
        }
        return outcome;
    }

    void treeNavigated(const std::optional<std::string>& newLeafId, const std::optional<std::string>& oldLeafId, const std::optional<SessionEntry>& summaryEntry, bool fromExtension) {
        Json payload = Json::object({{"newLeafId", optionalString(newLeafId)}, {"oldLeafId", optionalString(oldLeafId)}});
        if (summaryEntry) {
            payload["summaryEntry"] = summaryEntry->body;
            payload["fromExtension"] = fromExtension;
        }
        observe("session_tree", payload);
    }

    /** False when a handler of `session_before_switch` cancelled the new session or the switch. */
    bool allowSwitch(const std::string& reason, const std::optional<std::string>& targetSessionFile) {
        Json payload = Json::object({{"reason", reason}});
        if (targetSessionFile) {
            payload["targetSessionFile"] = *targetSessionFile;
        }
        return !cancelled("session_before_switch", payload);
    }

    /** False when a handler of `session_before_fork` cancelled the fork. */
    bool allowFork(const std::string& entryId, const std::string& position) {
        return !cancelled("session_before_fork", Json::object({{"entryId", entryId}, {"position", position}}));
    }

    /** {trusted, remember} of the first `project_trust` handler that decided; nullopt when none did. */
    std::optional<std::pair<bool, bool>> projectTrust(const std::string& cwd) {
        if (!m_bus.hasHandlers("project_trust")) {
            return std::nullopt;
        }
        const HookOutcome emitted = m_bus.emit("project_trust", Json::object({{"type", "project_trust"}, {"cwd", cwd}}));
        for (const Json& answer : emitted.results) {
            if (!answer.is_object() || !answer.contains("trusted") || !answer["trusted"].is_string()) {
                continue;
            }
            const std::string verdict = answer["trusted"].get<std::string>();
            if (verdict == "yes" || verdict == "no") {
                return std::make_pair(verdict == "yes", answer.value("remember", false));
            }
        }
        return std::nullopt;
    }

    /** The skill and prompt template paths `resources_discover` handlers added (first: skills, second: prompt templates). */
    std::pair<std::vector<std::string>, std::vector<std::string>> resourcesDiscover(const std::string& cwd, const std::string& reason) {
        std::pair<std::vector<std::string>, std::vector<std::string>> paths;
        if (!m_bus.hasHandlers("resources_discover")) {
            return paths;
        }
        const HookOutcome emitted = m_bus.emit("resources_discover", Json::object({{"type", "resources_discover"}, {"cwd", cwd}, {"reason", reason}}));
        for (const Json& answer : emitted.results) {
            collectPaths(answer, "skillPaths", paths.first);
            collectPaths(answer, "promptPaths", paths.second);
        }
        return paths;
    }

    /** The result a `user_bash` handler supplies instead of running `command`; nullopt when none did. */
    std::optional<BashResult> userBash(const std::string& command, bool excludeFromContext, const std::string& cwd) {
        if (!m_bus.hasHandlers("user_bash")) {
            return std::nullopt;
        }
        const HookOutcome emitted = m_bus.emit("user_bash", Json::object({{"type", "user_bash"}, {"command", command}, {"excludeFromContext", excludeFromContext}, {"cwd", cwd}}));
        for (const Json& answer : emitted.results) {
            if (!answer.is_object() || !answer.contains("result") || !answer["result"].is_object()) {
                continue;
            }
            const Json& result = answer["result"];
            BashResult out;
            out.output = result.value("output", std::string());
            if (result.contains("exitCode") && result["exitCode"].is_number_integer()) {
                out.exitCode = result["exitCode"].get<int>();
            }
            out.cancelled = result.value("cancelled", false);
            out.truncated = result.value("truncated", false);
            if (result.contains("fullOutputPath") && result["fullOutputPath"].is_string()) {
                out.fullOutputPath = result["fullOutputPath"].get<std::string>();
            }
            return out;
        }
        return std::nullopt;
    }

    void afterProviderResponse(int status, const Json& headers) {
        observe("after_provider_response", Json::object({{"status", status}, {"headers", headers}}));
    }

    void providerStreamEvent(const std::string& provider, const std::string& api, const std::string& model, const Json& data) {
        observe("provider_stream_event", Json::object({{"provider", provider}, {"api", api}, {"model", model}, {"data", data}}));
    }

    void modelSelected(const Json& model, const Json& previousModel, const std::string& source) {
        Json payload = Json::object({{"model", model}, {"source", source}});
        if (!previousModel.is_null()) {
            payload["previousModel"] = previousModel;
        }
        observe("model_select", payload);
    }

    void thinkingLevelSelected(const std::string& level, const std::string& previousLevel) {
        observe("thinking_level_select", Json::object({{"level", level}, {"previousLevel", previousLevel}}));
    }

private:
    void collectPaths(const Json& answer, const std::string& key, std::vector<std::string>& out) const {
        if (!answer.is_object() || !answer.contains(key) || !answer[key].is_array()) {
            return;
        }
        for (const Json& path : answer[key]) {
            if (path.is_string()) {
                out.push_back(path.get<std::string>());
            }
        }
    }

    bool cancelled(const std::string& event, Json payload) {
        if (!m_bus.hasHandlers(event)) {
            return false;
        }
        payload["type"] = event;
        const HookOutcome emitted = m_bus.emit(event, payload);
        return std::ranges::any_of(emitted.results, [](const Json& result) { return result.is_object() && result.value("cancel", false); });
    }

    void observe(const std::string& event, Json payload) {
        if (m_bus.hasHandlers(event)) {
            payload["type"] = event;
            m_bus.emit(event, payload);
        }
    }

    /** {customType, content, display, details} with the defaults of a custom message: no content is an empty list. */
    Json normalizedMessage(const Json& message) const {
        const bool typed = message.contains("customType") && message["customType"].is_string();
        const bool shown = !message.contains("display") || !message["display"].is_boolean() || message["display"].get<bool>();
        return Json::object({{"customType", typed ? message["customType"] : Json("")},
                             {"content", message.contains("content") && !message["content"].is_null() ? message["content"] : Json::array()},
                             {"display", shown},
                             {"details", message.contains("details") ? message["details"] : Json()}});
    }

    Json optionalString(const std::optional<std::string>& value) const {
        return value ? Json(*value) : Json();
    }

    Json imagesJson(const std::vector<ImageContent>& images) const {
        Json out = Json::array();
        for (const ImageContent& image : images) {
            out.push_back(m_messages.toJson(image));
        }
        return out;
    }

    std::vector<ImageContent> imagesFrom(const Json& json, const std::vector<ImageContent>& fallback) const {
        if (!json.is_array()) {
            return fallback;
        }
        std::vector<ImageContent> images;
        for (const Json& entry : json) {
            const auto block = m_messages.userBlockFromJson(entry);
            if (block) {
                if (const auto* image = std::get_if<ImageContent>(&*block)) {
                    images.push_back(*image);
                }
            }
        }
        return images;
    }

    Json entriesJson(const std::vector<SessionEntry>& entries) const {
        Json out = Json::array();
        for (const SessionEntry& entry : entries) {
            out.push_back(entry.body);
        }
        return out;
    }

    Json pathsJson(const std::set<std::string>& paths) const {
        Json out = Json::array();
        for (const std::string& path : paths) {
            out.push_back(path);
        }
        return out;
    }

    Json preparationJson(const CompactionPreparation& preparation) const {
        Json out = Json::object({{"firstKeptEntryId", preparation.firstKeptEntryId},
                                 {"messagesToSummarize", m_agentMessages.listToJson(preparation.messagesToSummarize)},
                                 {"turnPrefixMessages", m_agentMessages.listToJson(preparation.turnPrefixMessages)},
                                 {"isSplitTurn", preparation.isSplitTurn},
                                 {"tokensBefore", preparation.tokensBefore},
                                 {"fileOps", Json::object({{"read", pathsJson(preparation.fileOps.read)},
                                                           {"written", pathsJson(preparation.fileOps.written)},
                                                           {"edited", pathsJson(preparation.fileOps.edited)}})},
                                 {"settings", Json::object({{"enabled", preparation.settings.enabled},
                                                            {"reserveTokens", preparation.settings.reserveTokens},
                                                            {"keepRecentTokens", preparation.settings.keepRecentTokens}})}});
        if (preparation.previousSummary) {
            out["previousSummary"] = *preparation.previousSummary;
        }
        return out;
    }

    std::optional<CompactionResult> compactionFrom(const Json& result) const {
        if (!result.contains("compaction") || !result["compaction"].is_object()) {
            return std::nullopt;
        }
        const Json& json = result["compaction"];
        if (!json.contains("summary") || !json["summary"].is_string() || !json.contains("firstKeptEntryId") ||
            !json["firstKeptEntryId"].is_string()) {
            return std::nullopt;
        }
        CompactionResult compaction;
        compaction.summary = json["summary"].get<std::string>();
        compaction.firstKeptEntryId = json["firstKeptEntryId"].get<std::string>();
        if (json.contains("tokensBefore") && json["tokensBefore"].is_number_integer()) {
            compaction.tokensBefore = json["tokensBefore"].get<std::int64_t>();
        }
        if (json.contains("details")) {
            compaction.details = json["details"];
        }
        if (json.contains("usage") && json["usage"].is_object()) {
            if (auto usage = m_messages.usageFromJson(json["usage"])) {
                compaction.usage = *usage;
            }
        }
        return compaction;
    }

    void mergeTree(const Json& result, BeforeTreeOutcome& outcome) const {
        if (result.contains("summary") && result["summary"].is_object() && result["summary"].contains("summary") &&
            result["summary"]["summary"].is_string()) {
            const Json& summary = result["summary"];
            outcome.summary = summary["summary"].get<std::string>();
            outcome.details = summary.contains("details") ? summary["details"] : Json();
            outcome.usage.reset();
            if (summary.contains("usage") && summary["usage"].is_object()) {
                if (auto usage = m_messages.usageFromJson(summary["usage"])) {
                    outcome.usage = *usage;
                }
            }
        }
        if (result.contains("customInstructions") && result["customInstructions"].is_string()) {
            outcome.customInstructions = result["customInstructions"].get<std::string>();
        }
        if (result.contains("replaceInstructions") && result["replaceInstructions"].is_boolean()) {
            outcome.replaceInstructions = result["replaceInstructions"].get<bool>();
        }
        if (result.contains("label") && result["label"].is_string()) {
            outcome.label = result["label"].get<std::string>();
        }
    }

    IHookBus& m_bus;
    MessageCodec m_messages;
    AgentMessageCodec m_agentMessages;
};
