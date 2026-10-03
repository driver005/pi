export module pi.support.conversation_serializer;

import std;
import pi.support.json_writer;
export import pi.types.message;

/**
 * Renders provider messages as plain text for a summarization request, so the summarizer reads
 * the conversation instead of continuing it. Tool results are cut to keep the request small.
 * Port of serializeConversation in compaction/utils.ts.
 */
export class ConversationSerializer {
public:
    std::string serialize(const std::vector<Message>& messages) const {
        std::vector<std::string> parts;
        for (const auto& message : messages) {
            if (const auto* user = std::get_if<UserMessage>(&message)) {
                if (const std::string text = userText(*user); !text.empty()) {
                    parts.push_back("[User]: " + text);
                }
            } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
                appendAssistant(*assistant, parts);
            } else if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
                if (const std::string text = blocksText(result->content, ""); !text.empty()) {
                    parts.push_back("[Tool result]: " + truncate(text));
                }
            }
        }
        return join(parts, "\n\n");
    }

    /** System prompt shared by compaction and branch summarization. */
    std::string systemPrompt() const {
        return "You are a context summarization assistant. Your task is to read a conversation between a user and an AI assistant, then produce a structured summary following the exact format specified.\n\n"
               "Do NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the structured summary.";
    }

private:
    static constexpr std::size_t ToolResultMaxChars = 2000;

    std::string userText(const UserMessage& message) const {
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            return *text;
        }
        return blocksText(std::get<std::vector<UserContentBlock>>(message.content), "");
    }

    std::string blocksText(const std::vector<UserContentBlock>& blocks, const std::string& separator) const {
        std::vector<std::string> texts;
        for (const auto& block : blocks) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                texts.push_back(text->text);
            }
        }
        return join(texts, separator);
    }

    void appendAssistant(const AssistantMessage& message, std::vector<std::string>& parts) const {
        std::vector<std::string> thinking;
        std::vector<std::string> texts;
        std::vector<std::string> calls;
        for (const auto& block : message.content) {
            if (const auto* think = std::get_if<ThinkingContent>(&block)) {
                thinking.push_back(think->thinking);
            } else if (const auto* text = std::get_if<TextContent>(&block)) {
                texts.push_back(text->text);
            } else if (const auto* call = std::get_if<ToolCall>(&block)) {
                calls.push_back(toolCallText(*call));
            }
        }
        if (!thinking.empty()) {
            parts.push_back("[Assistant thinking]: " + join(thinking, "\n"));
        }
        if (!texts.empty()) {
            parts.push_back("[Assistant]: " + join(texts, "\n"));
        }
        if (!calls.empty()) {
            parts.push_back("[Assistant tool calls]: " + join(calls, "; "));
        }
    }

    std::string toolCallText(const ToolCall& call) const {
        std::vector<std::string> args;
        if (call.arguments.is_object()) {
            for (const auto& entry : call.arguments.items()) {
                const std::string& key = entry.key();
                const Json& value = entry.value();
                args.push_back(key + "=" + m_writer.compact(value));
            }
        }
        return call.name + "(" + join(args, ", ") + ")";
    }

    std::string truncate(const std::string& text) const {
        if (codePoints(text) <= ToolResultMaxChars) {
            return text;
        }
        std::size_t end = 0;
        std::size_t seen = 0;
        while (end < text.size() && seen < ToolResultMaxChars) {
            ++end;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                ++end;
            }
            ++seen;
        }
        const std::size_t dropped = codePoints(text) - ToolResultMaxChars;
        return text.substr(0, end) + "\n\n[... " + std::to_string(dropped) + " more characters truncated]";
    }

    std::size_t codePoints(const std::string& text) const {
        std::size_t count = 0;
        for (const unsigned char byte : text) {
            count += (byte & 0xC0) != 0x80 ? 1 : 0;
        }
        return count;
    }

    std::string join(const std::vector<std::string>& items, const std::string& separator) const {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            out += (i == 0 ? "" : separator) + items[i];
        }
        return out;
    }

    JsonWriter m_writer;
};
