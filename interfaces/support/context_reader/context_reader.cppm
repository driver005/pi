module;

#include <cstdint>

export module pi.support.context_reader;

import std;
export import pi.durable.i_storage;
export import pi.support.durable_session;
export import pi.types.context_bounds;
export import pi.types.json;
export import pi.types.result;

/**
 * Derives the active transcript and model context of a conversation from committed entries. Port of
 * packages/durable/src/harness/context.ts.
 *
 * H = newest visible head marker; the range runs from `H.head` (or the transcript start) through the tail. Per
 * target, the newest edit in the range wins. The context entries are H followed by the range's non-head entries.
 */
export class ContextReader {
public:
    static constexpr std::size_t kScanPageSize = 256;

    /**
     * Captures the bounds of the current context, or of the context cut off at the visible entry `at`, with two O(1)
     * reads. Run this on the session line; entries at or below the tail are immutable, so `derive` can scan them off
     * the line. Nothing when the conversation has no entries.
     */
    Result<std::optional<ContextBounds>> capture(IStorage& storage, std::int64_t conversationId,
                                                 const std::optional<std::int64_t>& at) const {
        std::int64_t tail = 0;
        if (!at) {
            auto page = storage.scanEntries(EntryQuery{conversationId, std::nullopt, std::nullopt}, 1, std::nullopt);
            if (!page) {
                return std::unexpected(page.error());
            }
            if (page->items.empty()) {
                return std::optional<ContextBounds>();
            }
            tail = page->items[0].at("id").get<std::int64_t>();
        } else {
            auto visible = storage.visibleEntry(conversationId, *at);
            if (!visible) {
                return std::unexpected(visible.error());
            }
            if (!*visible) {
                return std::unexpected(Error{"durable_error", "Entry " + std::to_string(*at) + " is not visible from conversation " +
                                                                  std::to_string(conversationId)});
            }
            tail = *at;
        }
        auto head = storage.findLatestHeadMarker(conversationId, tail);
        if (!head) {
            return std::unexpected(head.error());
        }
        return std::optional<ContextBounds>(ContextBounds{*head ? **head : Json(nullptr), tail});
    }

    /** The committed context of one conversation: bounds captured on the session line, entries derived off it. */
    Result<Json> read(DurableSession& session, std::int64_t conversationId, const std::optional<std::int64_t>& at) const {
        std::optional<ContextBounds> bounds;
        auto captured = session.readOnLine([&]() -> Result<void> {
            auto found = capture(session.storage(), conversationId, at);
            if (!found) {
                return std::unexpected(found.error());
            }
            bounds = *found;
            return {};
        });
        if (!captured) {
            return std::unexpected(captured.error());
        }
        return derive(session.storage(), conversationId, bounds);
    }

    /** `{head, entries, contributions, messages}` of one conversation within captured bounds. */
    Result<Json> derive(IStorage& storage, std::int64_t conversationId, const std::optional<ContextBounds>& bounds) const {
        if (!bounds) {
            return Json::object({{"head", nullptr}, {"entries", Json::array()}, {"contributions", Json::array()}, {"messages", Json::array()}});
        }
        auto range = scanRange(storage, conversationId, *bounds);
        if (!range) {
            return std::unexpected(range.error());
        }
        std::map<std::int64_t, Json> edits;
        // Edits of every entry in the range count, including older head markers that `selectActive` drops.
        for (const Json& entry : *range) {
            if (entry.contains("edits")) {
                for (const Json& edit : entry.at("edits")) {
                    edits[edit.at("target").get<std::int64_t>()] = edit;
                }
            }
        }
        const Json entries = selectActive(bounds->head, *range);
        Json contributions = Json::array();
        Json flat = Json::array();
        for (const Json& entry : entries) {
            Json contributed = contribution(entry, edits);
            for (const Json& message : contributed) {
                flat.push_back(message);
            }
            contributions.push_back(std::move(contributed));
        }
        return Json::object({{"head", bounds->head}, {"entries", entries}, {"contributions", contributions}, {"messages", orderToolResults(flat)}});
    }

    /** The raw active entries within captured bounds, without deriving model context. */
    Result<Json> activeEntries(IStorage& storage, std::int64_t conversationId, const std::optional<ContextBounds>& bounds) const {
        if (!bounds) {
            return Json::array();
        }
        auto range = scanRange(storage, conversationId, *bounds);
        if (!range) {
            return std::unexpected(range.error());
        }
        return selectActive(bounds->head, *range);
    }

    /**
     * Places each assistant's tool results directly after it in call order. Results are taken from the messages before
     * the next assistant; a missing result is synthesized and unmatched results are dropped.
     */
    Json orderToolResults(const Json& messages) const {
        Json ordered = Json::array();
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const Json& message = messages[index];
            const std::string role = message.value("role", std::string());
            if (role == "toolResult") {
                continue;
            }
            ordered.push_back(message);
            if (role != "assistant") {
                continue;
            }
            const std::vector<Json> calls = toolCalls(message);
            if (calls.empty()) {
                continue;
            }
            std::map<std::string, std::size_t> results;
            for (std::size_t next = index + 1; next < messages.size() && messages[next].value("role", std::string()) != "assistant"; ++next) {
                const Json& candidate = messages[next];
                if (candidate.value("role", std::string()) == "toolResult") {
                    results.emplace(candidate.value("toolCallId", std::string()), next);
                }
            }
            for (const Json& call : calls) {
                auto found = results.find(call.at("id").get<std::string>());
                ordered.push_back(found == results.end() ? missingResult(call, message.value("timestamp", std::int64_t(0))) : messages[found->second]);
            }
        }
        return ordered;
    }

private:
    /** The visible entries from the head marker's head, or the transcript start, through the tail, oldest first. */
    Result<std::vector<Json>> scanRange(IStorage& storage, std::int64_t conversationId, const ContextBounds& bounds) const {
        EntryQuery query{conversationId, std::nullopt, bounds.tail};
        if (!bounds.head.is_null()) {
            query.minEntryId = bounds.head.at("head").get<std::int64_t>();
        }
        std::vector<Json> range;
        std::optional<Json> cursor;
        do {
            auto page = storage.scanEntries(query, kScanPageSize, cursor);
            if (!page) {
                return std::unexpected(page.error());
            }
            range.insert(range.end(), page->items.begin(), page->items.end());
            cursor = page->next;
        } while (cursor);
        std::reverse(range.begin(), range.end());
        return range;
    }

    /** The head marker followed by the range's non-head entries, or the whole range without a marker. */
    Json selectActive(const Json& head, const std::vector<Json>& range) const {
        Json active = Json::array();
        if (head.is_null()) {
            for (const Json& entry : range) {
                active.push_back(entry);
            }
            return active;
        }
        active.push_back(head);
        for (const Json& entry : range) {
            if (!entry.contains("head")) {
                active.push_back(entry);
            }
        }
        return active;
    }

    Json contribution(const Json& entry, const std::map<std::int64_t, Json>& edits) const {
        auto edit = edits.find(entry.at("id").get<std::int64_t>());
        if (edit != edits.end() && edit->second.at("action") == "omit") {
            return Json::array();
        }
        const Json* source = nullptr;
        if (edit != edits.end() && edit->second.at("action") == "replace") {
            source = &edit->second.at("messages");
        } else if (entry.contains("model")) {
            source = &entry.at("model");
        }
        Json contributed = Json::array();
        if (source == nullptr) {
            return contributed;
        }
        for (const Json& message : *source) {
            if (message.value("role", std::string()) == "assistant" && excluded(message.value("stopReason", std::string()))) {
                continue;
            }
            contributed.push_back(message);
        }
        return contributed;
    }

    bool excluded(const std::string& stopReason) const {
        return stopReason == "aborted" || stopReason == "error" || stopReason == "deferred";
    }

    std::vector<Json> toolCalls(const Json& assistant) const {
        std::vector<Json> calls;
        if (assistant.contains("content") && assistant.at("content").is_array()) {
            for (const Json& block : assistant.at("content")) {
                if (block.value("type", std::string()) == "toolCall") {
                    calls.push_back(block);
                }
            }
        }
        return calls;
    }

    Json missingResult(const Json& call, std::int64_t timestamp) const {
        return Json::object({{"role", "toolResult"},
                             {"toolCallId", call.at("id")},
                             {"toolName", call.at("name")},
                             {"content", Json::array({Json::object({{"type", "text"}, {"text", "Tool result unavailable: history ends before this call completed."}})})},
                             {"isError", true},
                             {"details", Json::object({{"reason", "missing_result"}})},
                             {"timestamp", timestamp}});
    }
};
