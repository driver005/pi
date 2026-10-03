module;

#include <cstdint>

export module pi.support.session_projector;

import std;
export import pi.support.agent_message_codec;
export import pi.support.iso_timestamp;
export import pi.types.session_context;
export import pi.types.session_entry;
export import pi.types.session_projection;

/**
 * Turns the session tree into model context: the path from the leaf to the root, shortened at
 * the latest compaction, with context edits applied. Port of the buildSessionContext family in
 * core/session-manager.ts.
 */
export class SessionProjector {
public:
    /** Root-to-leaf path. nullopt leaf means "before any entry" and yields an empty path. */
    std::vector<SessionEntry> buildPath(const std::vector<SessionEntry>& entries, const std::optional<std::string>& leafId) const {
        if (!leafId) {
            return {};
        }
        const SessionEntry* leaf = nullptr;
        for (const auto& entry : entries) {
            if (entry.id == *leafId) {
                leaf = &entry;
                break;
            }
        }
        if (leaf == nullptr && !entries.empty()) {
            leaf = &entries.back();
        }
        return walk(entries, leaf);
    }

    /** Root-to-last-entry path, for callers without an explicit leaf. */
    std::vector<SessionEntry> buildPathToEnd(const std::vector<SessionEntry>& entries) const {
        return walk(entries, entries.empty() ? nullptr : &entries.back());
    }

    /** Active entries: the latest compaction, the kept range, everything after it. */
    std::vector<SessionEntry> buildContextEntries(const std::vector<SessionEntry>& entries, const std::optional<std::string>& leafId) const {
        std::vector<SessionEntry> path = buildPath(entries, leafId);
        std::optional<std::size_t> compactionIndex;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i].type == "compaction") {
                compactionIndex = i;
            }
        }
        if (!compactionIndex) {
            return path;
        }
        const SessionEntry& compaction = path[*compactionIndex];
        const std::string firstKept = compaction.body.value("firstKeptEntryId", "");
        std::vector<SessionEntry> out = {compaction};
        bool foundFirstKept = false;
        for (std::size_t i = 0; i < *compactionIndex; ++i) {
            if (path[i].id == firstKept) {
                foundFirstKept = true;
            }
            const bool systemMessage = path[i].type == "message" && path[i].body.contains("message") &&
                                       path[i].body["message"].is_object() &&
                                       path[i].body["message"].value("role", "") == "system";
            if (foundFirstKept && !systemMessage) {
                out.push_back(path[i]);
            }
        }
        for (std::size_t i = *compactionIndex + 1; i < path.size(); ++i) {
            out.push_back(path[i]);
        }
        return out;
    }

    /** Messages one entry contributes (state-only entries contribute none). */
    std::vector<AgentMessage> entryToMessages(const SessionEntry& entry) const {
        if (entry.type == "message") {
            return decode(messageJson(entry));
        }
        if (entry.type == "custom_message") {
            return decode(customMessage(entry));
        }
        if (entry.type == "branch_summary" && entry.body.contains("summary") && entry.body["summary"].is_string() &&
            !entry.body["summary"].get<std::string>().empty()) {
            Json out = Json::object();
            out["role"] = "branchSummary";
            out["summary"] = entry.body["summary"];
            out["fromId"] = entry.body.contains("fromId") ? entry.body["fromId"] : Json(nullptr);
            out["timestamp"] = timestampMs(entry);
            return decode(out);
        }
        if (entry.type == "compaction") {
            Json out = Json::object();
            out["role"] = "compactionSummary";
            out["summary"] = entry.body.value("summary", "");
            out["tokensBefore"] = entry.body.contains("tokensBefore") ? entry.body["tokensBefore"] : Json(0);
            out["timestamp"] = timestampMs(entry);
            std::vector<AgentMessage> messages;
            if (entry.body.contains("systemMessage") && entry.body["systemMessage"].is_object()) {
                Json system = entry.body["systemMessage"];
                system["role"] = "system";
                for (auto& message : decode(system)) {
                    messages.push_back(std::move(message));
                }
            }
            for (auto& message : decode(out)) {
                messages.push_back(std::move(message));
            }
            return messages;
        }
        return {};
    }

    SessionProjection project(const std::vector<SessionEntry>& entries, const std::optional<std::string>& leafId) const {
        SessionProjection out;
        applySettings(buildPath(entries, leafId), out);
        const std::vector<SessionEntry> contextEntries = buildContextEntries(entries, leafId);
        std::map<std::string, SessionEntry> edits;
        for (const auto& entry : contextEntries) {
            if (entry.type == "context_edit") {
                edits[entry.body.value("targetId", "")] = entry;
            }
        }
        for (std::size_t i = 0; i < contextEntries.size(); ++i) {
            const SessionEntry& source = contextEntries[i];
            ProjectedSessionEntry projected;
            projected.sourceEntry = source;
            // An older compaction can survive inside the kept range; only the newest, at index 0,
            // contributes a checkpoint and summary.
            if (!(source.type == "compaction" && i > 0)) {
                projected.messages = entryToMessages(source);
                const auto edit = edits.find(source.id);
                if (edit != edits.end()) {
                    projected.messages = applyEdit(std::move(projected.messages), edit->second);
                }
            }
            for (const auto& message : projected.messages) {
                out.messages.push_back(message);
            }
            out.entries.push_back(std::move(projected));
        }
        return out;
    }

    SessionContext context(const std::vector<SessionEntry>& entries, const std::optional<std::string>& leafId) const {
        SessionProjection projection = project(entries, leafId);
        SessionContext out;
        out.messages = std::move(projection.messages);
        out.thinkingLevel = std::move(projection.thinkingLevel);
        out.model = std::move(projection.model);
        return out;
    }

    std::optional<SessionEntry> latestCompaction(const std::vector<SessionEntry>& entries) const {
        for (std::size_t i = entries.size(); i-- > 0;) {
            if (entries[i].type == "compaction") {
                return entries[i];
            }
        }
        return std::nullopt;
    }

private:
    std::vector<SessionEntry> walk(const std::vector<SessionEntry>& entries, const SessionEntry* leaf) const {
        std::map<std::string, const SessionEntry*> index;
        for (const auto& entry : entries) {
            index[entry.id] = &entry;
        }
        std::vector<SessionEntry> path;
        std::set<std::string> seen;
        const SessionEntry* current = leaf;
        while (current != nullptr && seen.insert(current->id).second) {
            path.push_back(*current);
            if (!current->parentId) {
                break;
            }
            const auto parent = index.find(*current->parentId);
            current = parent == index.end() ? nullptr : parent->second;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    Json messageJson(const SessionEntry& entry) const {
        Json message = entry.body.contains("message") ? entry.body["message"] : Json::object();
        if (!message.is_object()) {
            return message;
        }
        // Hand-edited or very old files can hold null content; give it a harmless default.
        const std::string role = message.value("role", "");
        const bool missing = !message.contains("content") || message["content"].is_null();
        if (missing && role == "system") {
            message["content"] = "";
        } else if (missing && (role == "user" || role == "assistant" || role == "toolResult")) {
            message["content"] = Json::array();
        }
        return message;
    }

    std::vector<AgentMessage> decode(const Json& json) const {
        auto message = m_codec.fromJson(json);
        if (!message) {
            return {};
        }
        return {std::move(*message)};
    }

    std::vector<AgentMessage> applyEdit(std::vector<AgentMessage> messages, const SessionEntry& edit) const {
        if (!edit.body.contains("replacement") || edit.body["replacement"].is_null()) {
            return {};
        }
        const Json& replacement = edit.body["replacement"];
        if (!replacement.is_object() || !replacement.contains("content")) {
            return messages;
        }
        std::vector<AgentMessage> out;
        for (auto& message : messages) {
            const std::string role = m_codec.roleOf(message);
            if (role != "user" && role != "assistant" && role != "toolResult" && role != "custom") {
                out.push_back(std::move(message));
                continue;
            }
            Json json = m_codec.toJson(message);
            Json content = replacement["content"];
            if ((role == "assistant" || role == "toolResult") && content.is_string()) {
                Json block = Json::object();
                block["type"] = "text";
                block["text"] = content;
                content = Json::array({block});
            }
            json["content"] = std::move(content);
            auto rebuilt = m_codec.fromJson(json);
            out.push_back(rebuilt ? std::move(*rebuilt) : std::move(message));
        }
        return out;
    }

    void applySettings(const std::vector<SessionEntry>& path, SessionProjection& out) const {
        for (const auto& entry : path) {
            if (entry.type == "thinking_level_change") {
                out.thinkingLevel = entry.body.value("thinkingLevel", "off");
            } else if (entry.type == "model_change") {
                out.model = SessionModelRef{entry.body.value("provider", ""), entry.body.value("modelId", "")};
            } else if (entry.type == "message" && entry.body.contains("message") && entry.body["message"].is_object() &&
                       entry.body["message"].value("role", "") == "assistant") {
                const Json& message = entry.body["message"];
                out.model = SessionModelRef{message.value("provider", ""), message.value("model", "")};
            }
        }
    }

    std::int64_t timestampMs(const SessionEntry& entry) const {
        return m_iso.parse(entry.timestamp).value_or(0);
    }

    Json customMessage(const SessionEntry& entry) const {
        Json out = Json::object();
        out["role"] = "custom";
        out["customType"] = entry.body.value("customType", "");
        out["content"] = entry.body.contains("content") && !entry.body["content"].is_null() ? entry.body["content"]
                                                                                          : Json::array();
        out["display"] = entry.body.value("display", false);
        if (entry.body.contains("details") && !entry.body["details"].is_null()) {
            out["details"] = entry.body["details"];
        }
        out["timestamp"] = timestampMs(entry);
        return out;
    }

    AgentMessageCodec m_codec;
    IsoTimestamp m_iso;
};
