module;

#include <cstdint>

export module pi.support.session_info_builder;

import std;
export import pi.support.iso_timestamp;
export import pi.support.session_entry_codec;
export import pi.types.session_info;

/** Summarizes a session file's text for listings. Port of buildSessionInfo in session-manager.ts. */
export class SessionInfoBuilder {
public:
    /** nullopt when the first entry is not a session header. fileMtimeMs is the last resort for `modified`. */
    std::optional<SessionInfo> build(const std::string& path, const std::string& content, std::int64_t fileMtimeMs) const {
        std::optional<SessionHeader> header;
        SessionInfo info;
        info.path = path;
        std::optional<std::int64_t> lastActivity;
        std::vector<std::string> allMessages;
        for (const auto& json : m_codec.parseLines(content)) {
            if (!header) {
                if (!m_codec.isHeader(json)) {
                    return std::nullopt;
                }
                header = m_codec.headerFromJson(json);
                continue;
            }
            const std::string type = json.value("type", "");
            if (type == "session_info") {
                std::string name = json.contains("name") && json["name"].is_string() ? json["name"].get<std::string>() : "";
                const auto first = name.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    info.name.reset();
                } else {
                    info.name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
                }
            }
            if (type != "message" || !json.contains("message") || !json["message"].is_object()) {
                continue;
            }
            ++info.messageCount;
            if (const auto time = activityTime(json)) {
                lastActivity = std::max(lastActivity.value_or(0), *time);
            }
            const Json& message = json["message"];
            const std::string role = message.value("role", "");
            if (role != "user" && role != "assistant") {
                continue;
            }
            const std::string text = textOf(message);
            if (text.empty()) {
                continue;
            }
            allMessages.push_back(text);
            if (info.firstMessage.empty() && role == "user") {
                info.firstMessage = text;
            }
        }
        if (!header) {
            return std::nullopt;
        }
        info.id = header->id;
        info.cwd = header->cwd;
        info.parentSessionPath = header->parentSession;
        info.createdMs = m_iso.parse(header->timestamp).value_or(0);
        const auto headerTime = m_iso.parse(header->timestamp);
        info.modifiedMs = lastActivity && *lastActivity > 0 ? *lastActivity : headerTime.value_or(fileMtimeMs);
        if (info.firstMessage.empty()) {
            info.firstMessage = "(no messages)";
        }
        for (std::size_t i = 0; i < allMessages.size(); ++i) {
            info.allMessagesText += (i > 0 ? " " : "") + allMessages[i];
        }
        return info;
    }

private:
    std::string textOf(const Json& message) const {
        if (!message.contains("content")) {
            return "";
        }
        const Json& content = message["content"];
        if (content.is_string()) {
            return content.get<std::string>();
        }
        std::string text;
        bool first = true;
        if (content.is_array()) {
            for (const auto& block : content) {
                if (block.is_object() && block.value("type", "") == "text" && block.contains("text") &&
                    block["text"].is_string()) {
                    text += (first ? "" : " ") + block["text"].get<std::string>();
                    first = false;
                }
            }
        }
        return text;
    }

    std::optional<std::int64_t> activityTime(const Json& entry) const {
        const Json& message = entry["message"];
        const std::string role = message.value("role", "");
        if (role != "user" && role != "assistant") {
            return std::nullopt;
        }
        if (message.contains("timestamp") && message["timestamp"].is_number()) {
            return message["timestamp"].get<std::int64_t>();
        }
        return m_iso.parse(entry.value("timestamp", ""));
    }

    SessionEntryCodec m_codec;
    IsoTimestamp m_iso;
};
