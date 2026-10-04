export module pi.support.plugin_session_events;

import std;
export import pi.plugin.i_hook_bus;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.types.agent_start_outcome;
export import pi.types.before_compact_outcome;
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
 *    fromExtension?}: observation.
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

private:
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
