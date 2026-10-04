module;

#include <cstdint>

export module pi.support.pi_messages_stream_reader;

import std;
export import pi.platform.i_clock;
export import pi.support.assistant_stream_emitter;
export import pi.support.message_codec;
export import pi.types.json;
export import pi.types.model;
export import pi.types.result;
export import pi.types.sse_event;

/**
 * Replays the serialized assistant-message events of a pi-messages backend (Radius gateway or any
 * server speaking the protocol) through an AssistantStreamEmitter. Block positions chosen by the
 * backend are mapped onto the emitter's; text, thinking and tool call blocks take the final values
 * the backend sends with their end events, and the terminal done/error event carries usage,
 * response id and rewrite diagnostics. Port of createEventConverter in api/pi-messages.ts.
 */
export class PiMessagesStreamReader {
public:
    PiMessagesStreamReader(AssistantStreamEmitter& emitter, const IClock& clock)
        : m_emitter(emitter),
          m_clock(clock) {}

    Result<void> handle(const SseEvent& sse) {
        if (sse.data.empty() || sse.data == "[DONE]" || m_terminal) {
            return {};
        }
        const Json event = Json::parse(sse.data, nullptr, false);
        if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
            return std::unexpected(Error{"parse_error", "Invalid pi-messages event: " + sse.data});
        }
        const std::string type = event["type"].get<std::string>();
        const int position = event.contains("contentIndex") && event["contentIndex"].is_number_integer()
                                 ? event["contentIndex"].get<int>()
                                 : -1;
        if (type == "text_start") {
            m_blocks[position] = m_emitter.textStart();
        } else if (type == "text_delta") {
            if (const auto index = mapped(event)) {
                m_emitter.textDelta(*index, stringIn(event, "delta"));
            }
        } else if (type == "text_end") {
            textEnd(event);
        } else if (type == "thinking_start") {
            m_blocks[position] = m_emitter.thinkingStart();
        } else if (type == "thinking_delta") {
            if (const auto index = mapped(event)) {
                m_emitter.thinkingDelta(*index, stringIn(event, "delta"));
            }
        } else if (type == "thinking_end") {
            thinkingEnd(event);
        } else if (type == "toolcall_start") {
            toolCallStart(event);
        } else if (type == "toolcall_delta") {
            if (const auto index = mapped(event)) {
                m_emitter.toolCallDelta(*index, stringIn(event, "delta"));
            }
        } else if (type == "toolcall_end") {
            toolCallEnd(event);
        } else if (type == "done" || type == "error") {
            terminal(event, type == "error");
        }
        return {};
    }

    /** The stream must have delivered a terminal event; the provider name is for the message. */
    Result<void> finish(const std::string& provider) {
        if (!m_terminal) {
            return std::unexpected(Error{"stream_ended", provider + " stream ended without a terminal event"});
        }
        return {};
    }

private:
    void textEnd(const Json& event) {
        const auto index = mapped(event);
        if (!index) {
            return;
        }
        auto& block = std::get<TextContent>(m_emitter.message().content[static_cast<std::size_t>(*index)]);
        block.text = stringIn(event, "content");
        const std::string signature = stringIn(event, "contentSignature");
        block.textSignature = signature.empty() ? std::nullopt : std::optional<std::string>(signature);
        m_emitter.textEnd(*index);
    }

    void thinkingEnd(const Json& event) {
        const auto index = mapped(event);
        if (!index) {
            return;
        }
        auto& block = std::get<ThinkingContent>(m_emitter.message().content[static_cast<std::size_t>(*index)]);
        block.thinking = stringIn(event, "content");
        const std::string signature = stringIn(event, "contentSignature");
        block.thinkingSignature = signature.empty() ? std::nullopt : std::optional<std::string>(signature);
        if (event.contains("redacted") && event["redacted"].is_boolean()) {
            block.redacted = event["redacted"].get<bool>();
        }
        m_emitter.thinkingEnd(*index);
    }

    void toolCallStart(const Json& event) {
        const int position = event.contains("contentIndex") && event["contentIndex"].is_number_integer()
                                 ? event["contentIndex"].get<int>()
                                 : -1;
        m_blocks[position] = m_emitter.toolCallStart(stringIn(event, "id"), stringIn(event, "toolName"));
    }

    void toolCallEnd(const Json& event) {
        const auto index = mapped(event);
        if (!index) {
            return;
        }
        m_emitter.toolCallEnd(*index);
        if (event.contains("toolCall")) {
            if (auto call = m_codec.toolCallFromJson(event["toolCall"])) {
                std::get<ToolCall>(m_emitter.message().content[static_cast<std::size_t>(*index)]) = *call;
            }
        }
    }

    void terminal(const Json& event, bool failed) {
        m_terminal = true;
        AssistantMessage& message = m_emitter.message();
        if (event.contains("usage")) {
            if (auto usage = m_codec.usageFromJson(event["usage"])) {
                message.usage = *usage;
            }
        }
        const std::string responseId = stringIn(event, "responseId");
        if (!responseId.empty()) {
            message.responseId = responseId;
        }
        const std::string level = stringIn(event, "providerThinkingLevel");
        if (!level.empty()) {
            message.providerThinkingLevel = level;
        }
        appendRewriteDiagnostic(event.contains("rewrite") ? event["rewrite"] : Json());
        const auto reason = m_codec.parseStopReason(stringIn(event, "reason"));
        if (failed) {
            m_emitter.error(reason.value_or(StopReason::Error), stringIn(event, "errorMessage"));
        } else {
            m_emitter.done(reason.value_or(StopReason::Stop));
        }
    }

    void appendRewriteDiagnostic(const Json& rewrite) {
        if (!rewrite.is_object()) {
            return;
        }
        AssistantMessage& message = m_emitter.message();
        if (!message.diagnostics.is_array()) {
            message.diagnostics = Json::array();
        }
        message.diagnostics.push_back(
            Json{{"type", "pi_messages_rewrite"}, {"timestamp", m_clock.nowMs()}, {"details", rewrite}});
    }

    std::optional<int> mapped(const Json& event) const {
        if (!event.contains("contentIndex") || !event["contentIndex"].is_number_integer()) {
            return std::nullopt;
        }
        const auto found = m_blocks.find(event["contentIndex"].get<int>());
        return found == m_blocks.end() ? std::nullopt : std::optional<int>(found->second);
    }

    std::string stringIn(const Json& object, const std::string& key) const {
        return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
    }

    AssistantStreamEmitter& m_emitter;
    const IClock& m_clock;
    MessageCodec m_codec;
    std::map<int, int> m_blocks;
    bool m_terminal = false;
};
