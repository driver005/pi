export module pi.support.transcript_normalizer;

import std;
export import pi.types.context;
export import pi.types.message;
export import pi.types.system_message;
export import pi.types.tool;
export import pi.types.tool_state_changes;
export import pi.types.transcript_context;

/**
 * Transcript helpers: folding a Context into leading system messages, replaying system
 * messages into the current prompt and tool set, and diffing tool sets.
 * Port of packages/ai/src/utils/transcript.ts and the text helpers in utils/text.ts.
 */
export class TranscriptNormalizer {
public:
    /** Leading system message for a prompt and tool set; nullopt when both are empty. */
    std::optional<SystemMessage> createInitialSystemMessage(const std::optional<std::string>& systemPrompt, const std::optional<std::vector<Tool>>& tools) const {
        const bool hasPrompt = systemPrompt.has_value() && !systemPrompt->empty();
        const bool hasTools = tools.has_value() && !tools->empty();
        if (!hasPrompt && !hasTools) {
            return std::nullopt;
        }
        SystemMessage message;
        message.content = systemPrompt.value_or("");
        if (hasTools) {
            message.toolsAdded = *tools;
        }
        message.timestamp = 0;
        return message;
    }

    /** Folds Context::systemPrompt and Context::tools into a leading system message. */
    TranscriptContext normalizeContext(const Context& context) const {
        TranscriptContext out;
        if (auto initial = createInitialSystemMessage(context.systemPrompt, context.tools)) {
            out.messages.emplace_back(std::move(*initial));
        }
        out.messages.insert(out.messages.end(), context.messages.begin(), context.messages.end());
        return out;
    }

    /** The leading system message when the transcript starts with one. */
    const SystemMessage* initialSystemMessage(const std::vector<Message>& messages) const {
        if (messages.empty()) {
            return nullptr;
        }
        return std::get_if<SystemMessage>(&messages.front());
    }

    /** Tools available after applying every system message in order. */
    std::vector<Tool> currentTools(const std::vector<Message>& messages) const {
        std::vector<Tool> tools;
        for (const Message& message : messages) {
            const auto* system = std::get_if<SystemMessage>(&message);
            if (system == nullptr) {
                continue;
            }
            if (system->toolsRemoved.has_value()) {
                for (const ToolReference& removed : *system->toolsRemoved) {
                    tools.erase(std::remove_if(tools.begin(), tools.end(),
                                               [&](const Tool& tool) { return tool.name == removed.name; }),
                                tools.end());
                }
            }
            if (system->toolsAdded.has_value()) {
                for (const Tool& added : *system->toolsAdded) {
                    auto existing = std::find_if(tools.begin(), tools.end(),
                                                 [&](const Tool& tool) { return tool.name == added.name; });
                    if (existing != tools.end()) {
                        *existing = added;
                    } else {
                        tools.push_back(added);
                    }
                }
            }
        }
        return tools;
    }

    /** All system messages replayed into one leading message (content joined by blank lines). */
    std::optional<SystemMessage> currentSystemMessage(const std::vector<Message>& messages) const {
        std::vector<std::string> content;
        std::vector<std::pair<std::string, std::optional<std::string>>> sections;
        std::optional<std::int64_t> timestamp;
        for (const Message& message : messages) {
            const auto* system = std::get_if<SystemMessage>(&message);
            if (system == nullptr) {
                continue;
            }
            if (!timestamp.has_value()) {
                timestamp = system->timestamp;
            }
            if (const std::string text = systemContentText(*system); !text.empty()) {
                content.push_back(text);
            }
            if (!system->sections.has_value()) {
                continue;
            }
            for (const auto& [name, value] : *system->sections) {
                auto existing = std::find_if(sections.begin(), sections.end(),
                                             [&](const auto& entry) { return entry.first == name; });
                if (!value.has_value()) {
                    if (existing != sections.end()) {
                        sections.erase(existing);
                    }
                } else if (existing != sections.end()) {
                    existing->second = value;
                } else {
                    sections.emplace_back(name, value);
                }
            }
        }
        std::vector<Tool> tools = currentTools(messages);
        if (!timestamp.has_value() && tools.empty()) {
            return std::nullopt;
        }
        SystemMessage head;
        std::string joined;
        for (const std::string& part : content) {
            joined += (joined.empty() ? "" : "\n\n") + part;
        }
        head.content = joined;
        if (!sections.empty()) {
            head.sections = sections;
        }
        if (!tools.empty()) {
            head.toolsAdded = std::move(tools);
        }
        head.timestamp = timestamp.value_or(0);
        return head;
    }

    std::string currentSystemPrompt(const std::vector<Message>& messages) const {
        const auto message = currentSystemMessage(messages);
        return message.has_value() ? systemMessageText(*message) : "";
    }

    /** For APIs without mid-conversation system messages: replayed head, later ones dropped. */
    TranscriptContext collapseSystemMessages(const TranscriptContext& context) const {
        TranscriptContext out;
        if (auto head = currentSystemMessage(context.messages)) {
            out.messages.emplace_back(std::move(*head));
        }
        for (const Message& message : context.messages) {
            if (!std::holds_alternative<SystemMessage>(message)) {
                out.messages.push_back(message);
            }
        }
        return out;
    }

    TranscriptContext resolveTranscript(const TranscriptContext& context, bool supportsMidConvoSystemMessages) const {
        return supportsMidConvoSystemMessages ? context : collapseSystemMessages(context);
    }

    Tool toToolDeclaration(const Tool& tool) const {
        return Tool{tool.name, tool.description, tool.parameters, tool.constrainedSampling};
    }

    bool declarationsEqual(const Tool& left, const Tool& right) const {
        return left.name == right.name && left.description == right.description &&
               left.parameters.dump() == right.parameters.dump() &&
               left.constrainedSampling.dump() == right.constrainedSampling.dump();
    }

    ToolStateChanges toolStateChanges(const std::vector<Tool>& previous, const std::vector<Tool>& current) const {
        ToolStateChanges changes;
        auto findIn = [](const std::vector<Tool>& tools, const std::string& name) -> const Tool* {
            const auto found = std::find_if(tools.begin(), tools.end(),
                                            [&](const Tool& tool) { return tool.name == name; });
            return found == tools.end() ? nullptr : &*found;
        };
        for (const Tool& tool : current) {
            const Tool* before = findIn(previous, tool.name);
            if (before == nullptr || !declarationsEqual(*before, tool)) {
                changes.toolsAdded.push_back(toToolDeclaration(tool));
            }
        }
        for (const Tool& tool : previous) {
            const Tool* after = findIn(current, tool.name);
            if (after == nullptr || !declarationsEqual(tool, *after)) {
                changes.toolsRemoved.push_back({tool.name});
            }
        }
        return changes;
    }

    /** Joined text blocks of a system/user content value. */
    std::string systemContentText(const SystemMessage& message) const {
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            return *text;
        }
        std::string joined;
        for (const TextContent& block : std::get<std::vector<TextContent>>(message.content)) {
            joined += (joined.empty() ? "" : "\n") + block.text;
        }
        return joined;
    }

    /** A system message as a complete prompt: content followed by its sections. */
    std::string systemMessageText(const SystemMessage& message) const {
        std::vector<std::string> parts{systemContentText(message)};
        if (message.sections.has_value()) {
            for (const auto& [name, value] : *message.sections) {
                if (value.has_value()) {
                    parts.push_back(*value);
                }
            }
        }
        std::string joined;
        for (const std::string& part : parts) {
            if (!part.empty()) {
                joined += (joined.empty() ? "" : "\n\n") + part;
            }
        }
        return joined;
    }

    /** Later system message rendered for mid-conversation delivery (sections framed by name). */
    std::string renderSystemMessageUpdate(const SystemMessage& message) const {
        std::vector<std::string> parts;
        if (const std::string text = systemContentText(message); !text.empty()) {
            parts.push_back(text);
        }
        if (message.sections.has_value()) {
            for (const auto& [name, value] : *message.sections) {
                parts.push_back(value.has_value()
                                    ? "Updated system prompt section \"" + name + "\":\n\n" + *value
                                    : "Removed system prompt section \"" + name + "\".");
            }
        }
        std::string joined;
        for (const std::string& part : parts) {
            joined += (joined.empty() ? "" : "\n\n") + part;
        }
        return joined;
    }
};
