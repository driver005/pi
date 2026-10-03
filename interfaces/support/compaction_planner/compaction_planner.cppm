module;

#include <cstdint>

export module pi.support.compaction_planner;

import std;
export import pi.support.context_reader;
export import pi.support.live_editor;
export import pi.support.builtin_documents;
export import pi.support.message_codec;
export import pi.support.token_estimator;
export import pi.support.transaction;
export import pi.types.json;
export import pi.types.result;
export import pi.types.task_options;

/**
 * Range selection, sizing and summarization prompts of durable compaction (spec §8.7). Contexts are the JSON views of
 * ContextReader. Port of the pure parts of packages/durable/src/harness/compaction.ts.
 */
export class CompactionPlanner {
public:
    static constexpr std::size_t kToolResultMaxChars = 2000;

    std::string taskKind() const {
        return "pi.compaction";
    }

    std::string summaryPrefix() const {
        return "The conversation history before this point was compacted into the following summary:\n\n<summary>\n";
    }

    std::string summarySuffix() const {
        return "\n</summary>";
    }

    std::string systemPrompt() const {
        return "You are a context summarization assistant. Your task is to read a conversation between a user and an AI "
               "assistant, then produce a structured summary following the exact format specified.\n\n"
               "Do NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the "
               "structured summary.";
    }

    /**
     * Creates a compaction task with its status in this commit. `owner` is the generation that waits for it (a blocking
     * compaction); without one it is conversation-owned, and `background` unless it is manual. `input` is
     * `{reason, instructions?}`.
     */
    Result<std::int64_t> createCompaction(Transaction& tx, std::int64_t conversationId, const Json& input,
                                          const std::optional<std::int64_t>& owner = std::nullopt) const {
        TaskOptions options;
        options.ownership = owner ? Json::object({{"kind", "task"}, {"taskId", *owner}}) : Json::object({{"kind", "conversation"}});
        options.conversationId = conversationId;
        options.background = !owner && input.at("reason") != "manual";
        auto taskId = tx.createTask(taskKind(), 1, input, Json::object({{"phase", "select"}}), options);
        if (!taskId) {
            return std::unexpected(taskId.error());
        }
        DocAddressArgs args;
        args.owner = conversationId;
        auto live = tx.doc(m_documents.live(), args);
        if (!live) {
            return std::unexpected(live.error());
        }
        m_live.addCompactionStatus(**live, Json::object({{"taskId", *taskId}, {"reason", input.at("reason")},
                                                         {"blocking", owner.has_value()}, {"attempt", 1}}));
        return *taskId;
    }

    /**
     * The index in `view.entries` of the first entry a summary keeps, or nothing when there is nothing to compact. Walks
     * back from the tail until `keepRecentTokens` are kept, then cuts at the first candidate at or after that entry: an
     * entry whose contribution starts with a user or assistant message, never a tool result, and never a user entry that
     * a result of the preceding assistant's calls still follows.
     */
    std::optional<std::size_t> selectCut(const Json& view, std::int64_t keepRecentTokens) const {
        const Json& contributions = view.at("contributions");
        const std::size_t start = view.at("head").is_null() ? 0 : 1;
        std::vector<std::size_t> candidates;
        for (std::size_t index = start; index < contributions.size(); ++index) {
            if (isCandidate(contributions, index)) {
                candidates.push_back(index);
            }
        }
        std::int64_t kept = 0;
        std::optional<std::size_t> cut;
        for (std::size_t index = contributions.size(); index > start; --index) {
            for (const Json& message : contributions[index - 1]) {
                kept += messageTokens(message);
            }
            if (kept < keepRecentTokens) {
                continue;
            }
            for (const std::size_t candidate : candidates) {
                if (candidate >= index - 1) {
                    cut = candidate;
                    break;
                }
            }
            if (!cut && !candidates.empty()) {
                cut = candidates.back();
            }
            break;
        }
        if (!cut) {
            return std::nullopt;
        }
        for (std::size_t index = start; index < *cut; ++index) {
            if (!contributions[index].empty()) {
                return cut;
            }
        }
        return std::nullopt;
    }

    /** Model messages of the entries before `cut`: the head marker first, ordered like model context. */
    Json summarizedMessages(const Json& view, std::size_t cut) const {
        Json flat = Json::array();
        const Json& contributions = view.at("contributions");
        for (std::size_t index = 0; index < cut && index < contributions.size(); ++index) {
            for (const Json& message : contributions[index]) {
                flat.push_back(message);
            }
        }
        return m_context.orderToolResults(flat);
    }

    /**
     * The size of a request over `view` followed by `extra`: the usage of the newest assistant appended after the head
     * marker, whose request included the marker, plus estimates of the messages after it; without one, estimates of
     * every message.
     */
    std::int64_t estimateContext(const Json& view, const Json& extra) const {
        const Json& entries = view.at("entries");
        const Json& contributions = view.at("contributions");
        const Json& messages = view.at("messages");
        const std::int64_t after = view.at("head").is_null() ? std::numeric_limits<std::int64_t>::min() : view.at("head").at("id").get<std::int64_t>();
        std::optional<Json> measured;
        for (std::size_t index = entries.size(); index > 0 && !measured; --index) {
            if (entries[index - 1].at("id").get<std::int64_t>() <= after) {
                continue;
            }
            const Json& contribution = contributions[index - 1];
            for (std::size_t m = contribution.size(); m > 0; --m) {
                const Json& message = contribution[m - 1];
                if (message.value("role", std::string()) == "assistant" && usageTokens(message) > 0) {
                    measured = message;
                    break;
                }
            }
        }
        std::size_t from = 0;
        std::int64_t tokens = 0;
        if (measured) {
            for (std::size_t i = messages.size(); i > 0; --i) {
                if (messages[i - 1] == *measured) {
                    from = i;
                    break;
                }
            }
            tokens = usageTokens(*measured);
        }
        for (std::size_t i = from; i < messages.size(); ++i) {
            tokens += messageTokens(messages[i]);
        }
        for (const Json& message : extra) {
            tokens += messageTokens(message);
        }
        return tokens;
    }

    /** The summarizer's user message: the serialized conversation, the prompt, and any instructions. */
    std::string summaryPrompt(const Json& messages, const std::optional<std::string>& instructions) const {
        const std::string focus = instructions ? "\n\nAdditional focus: " + *instructions : "";
        return "<conversation>\n" + serializeConversation(messages) + "\n</conversation>\n\n" + instructionsPrompt() + focus;
    }

    /** Messages as plain text, so the summarizer reads a transcript instead of continuing it. System messages are omitted. */
    std::string serializeConversation(const Json& messages) const {
        std::vector<std::string> parts;
        for (const Json& message : messages) {
            const std::string role = message.value("role", std::string());
            if (role == "user") {
                const std::string text = contentText(message.at("content"));
                if (!text.empty()) {
                    parts.push_back("[User]: " + text);
                }
            } else if (role == "assistant") {
                serializeAssistant(message, parts);
            } else if (role == "toolResult") {
                const std::string text = contentText(message.at("content"));
                if (!text.empty()) {
                    parts.push_back("[Tool result]: " + truncate(text, kToolResultMaxChars));
                }
            }
        }
        std::string joined;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            joined += (i == 0 ? "" : "\n\n") + parts[i];
        }
        return joined;
    }

    /** The summary of a clean `stop` with text and no tool call; anything else is not a summary. */
    std::optional<std::string> summaryText(const Json& message) const {
        if (message.value("stopReason", std::string()) != "stop" || hasToolCall(message)) {
            return std::nullopt;
        }
        std::string text;
        for (const Json& block : message.at("content")) {
            if (block.value("type", std::string()) == "text") {
                text += (text.empty() ? "" : "\n") + block.value("text", std::string());
            }
        }
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::nullopt;
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string summaryFailure(const Json& message) const {
        const std::string stop = message.value("stopReason", std::string());
        if (stop == "error" || stop == "aborted") {
            return "Summarization failed: " + message.value("errorMessage", stop);
        }
        if (stop == "length") {
            return "Summarization hit the token limit; the summary is incomplete";
        }
        if (hasToolCall(message)) {
            return "Summarization attempted to call a tool";
        }
        return "Summarization produced no text";
    }

    /** The tokens of one JSON message; an unparseable one counts its serialized size. */
    std::int64_t messageTokens(const Json& message) const {
        auto typed = m_codec.messageFromJson(message);
        return typed ? m_estimator.messageTokens(*typed) : static_cast<std::int64_t>((message.dump().size() + 3) / 4);
    }

private:
    std::string instructionsPrompt() const {
        return "The messages above are a conversation to summarize. Create a structured context checkpoint summary that "
               "another LLM will use to continue the work. If the conversation starts with an earlier summary, preserve its "
               "information and fold the newer messages into it.\n\n"
               "Use this EXACT format:\n\n"
               "## Goal\n[What is the user trying to accomplish? Can be multiple items if the session covers different tasks.]\n\n"
               "## Constraints & Preferences\n- [Any constraints, preferences, or requirements mentioned by user]\n"
               "- [Or \"(none)\" if none were mentioned]\n\n"
               "## Progress\n### Done\n- [x] [Completed tasks/changes]\n\n### In Progress\n- [ ] [Current work]\n\n"
               "### Blocked\n- [Issues preventing progress, if any]\n\n"
               "## Key Decisions\n- **[Decision]**: [Brief rationale]\n\n"
               "## Next Steps\n1. [Ordered list of what should happen next]\n\n"
               "## Critical Context\n- [Any data, examples, or references needed to continue]\n"
               "- [Or \"(none)\" if not applicable]\n\n"
               "Keep each section concise. Preserve exact file paths, function names, and error messages.";
    }

    std::int64_t usageTokens(const Json& assistant) const {
        if (!assistant.contains("usage")) {
            return 0;
        }
        auto usage = m_codec.usageFromJson(assistant.at("usage"));
        return usage ? m_estimator.contextTokens(*usage) : 0;
    }

    bool hasToolCall(const Json& assistant) const {
        for (const Json& block : assistant.at("content")) {
            if (block.value("type", std::string()) == "toolCall") {
                return true;
            }
        }
        return false;
    }

    /** Whether an entry whose contribution starts at `index` can be the first entry a summary keeps. */
    bool isCandidate(const Json& contributions, std::size_t index) const {
        if (contributions[index].empty()) {
            return false;
        }
        const std::string first = contributions[index][0].value("role", std::string());
        if (first == "assistant") {
            return true;
        }
        if (first != "user") {
            return false;
        }
        // A result of the preceding assistant's calls that follows this entry, before the next assistant, belongs before it.
        std::set<std::string> calls = precedingCalls(contributions, index);
        if (calls.empty()) {
            return true;
        }
        for (std::size_t after = index; after < contributions.size(); ++after) {
            for (std::size_t position = 0; position < contributions[after].size(); ++position) {
                const Json& message = contributions[after][position];
                const std::string role = message.value("role", std::string());
                if (role == "assistant" && (after > index || position > 0)) {
                    return true;
                }
                if (role == "toolResult" && calls.contains(message.value("toolCallId", std::string()))) {
                    return false;
                }
            }
        }
        return true;
    }

    std::set<std::string> precedingCalls(const Json& contributions, std::size_t index) const {
        std::set<std::string> calls;
        for (std::size_t before = index; before > 0; --before) {
            const Json& contribution = contributions[before - 1];
            for (std::size_t m = contribution.size(); m > 0; --m) {
                if (contribution[m - 1].value("role", std::string()) != "assistant") {
                    continue;
                }
                for (const Json& block : contribution[m - 1].at("content")) {
                    if (block.value("type", std::string()) == "toolCall") {
                        calls.insert(block.at("id").get<std::string>());
                    }
                }
                return calls;
            }
        }
        return calls;
    }

    void serializeAssistant(const Json& message, std::vector<std::string>& parts) const {
        std::vector<std::string> thinking;
        std::vector<std::string> text;
        std::vector<std::string> calls;
        for (const Json& block : message.at("content")) {
            const std::string type = block.value("type", std::string());
            if (type == "thinking") {
                thinking.push_back(block.value("thinking", std::string()));
            } else if (type == "text") {
                text.push_back(block.value("text", std::string()));
            } else if (type == "toolCall") {
                std::string call = block.at("name").get<std::string>() + "(";
                bool first = true;
                if (block.contains("arguments") && block.at("arguments").is_object()) {
                    for (const auto& argument : block.at("arguments").items()) {
                        call += (first ? "" : ", ") + argument.key() + "=" + argument.value().dump();
                        first = false;
                    }
                }
                calls.push_back(call + ")");
            }
        }
        if (!thinking.empty()) {
            parts.push_back("[Assistant thinking]: " + join(thinking, "\n"));
        }
        if (!text.empty()) {
            parts.push_back("[Assistant]: " + join(text, "\n"));
        }
        if (!calls.empty()) {
            parts.push_back("[Assistant tool calls]: " + join(calls, "; "));
        }
    }

    std::string join(const std::vector<std::string>& items, const std::string& separator) const {
        std::string joined;
        for (std::size_t i = 0; i < items.size(); ++i) {
            joined += (i == 0 ? "" : separator) + items[i];
        }
        return joined;
    }

    std::string contentText(const Json& content) const {
        if (content.is_string()) {
            return content.get<std::string>();
        }
        std::vector<std::string> texts;
        for (const Json& block : content) {
            if (block.value("type", std::string()) == "text" && block.contains("text")) {
                texts.push_back(block.at("text").get<std::string>());
            }
        }
        return join(texts, "\n");
    }

    /** At most `maxChars` characters, cut on a character boundary, with a note of what was dropped. */
    std::string truncate(const std::string& text, std::size_t maxChars) const {
        std::size_t chars = 0;
        std::size_t cutAt = text.size();
        for (std::size_t i = 0; i < text.size(); ++i) {
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
                if (chars == maxChars) {
                    cutAt = i;
                    break;
                }
                ++chars;
            }
        }
        if (cutAt == text.size()) {
            return text;
        }
        std::size_t total = 0;
        for (const char c : text) {
            total += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
        return text.substr(0, cutAt) + "\n\n[... " + std::to_string(total - maxChars) + " more characters truncated]";
    }

    BuiltinDocuments m_documents;
    LiveEditor m_live;
    ContextReader m_context;
    MessageCodec m_codec;
    TokenEstimator m_estimator;
};
