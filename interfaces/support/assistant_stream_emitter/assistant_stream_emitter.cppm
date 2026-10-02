module;

#include <nlohmann/json.hpp>

export module pi.support.assistant_stream_emitter;

import std;
export import pi.provider.i_provider;
export import pi.support.partial_json_parser;
export import pi.types.assistant_message;

/**
 * Builds an AssistantMessage block by block and publishes the matching stream events with a
 * snapshot of the response so far. Every provider drives one of these from its own thread.
 * Block indices returned by the *Start methods are positions in message().content.
 */
export class AssistantStreamEmitter {
public:
    explicit AssistantStreamEmitter(AssistantMessage initial);

    /** The stream consumers read from; hand it out before starting to emit. */
    std::shared_ptr<AssistantMessageStream> stream() const;

    /** The response being built; providers set usage, responseId, ... directly. */
    AssistantMessage& message();

    void start();

    int textStart();
    void textDelta(int index, const std::string& delta);
    void textEnd(int index);

    int thinkingStart();
    void thinkingDelta(int index, const std::string& delta);
    void thinkingEnd(int index);

    int toolCallStart(const std::string& id, const std::string& name);
    void toolCallDelta(int index, const std::string& jsonFragment);
    /** Finalizes the arguments from the accumulated JSON text (repair, then partial parse). */
    void toolCallEnd(int index);

    /** Terminal success. reason: Stop, Length, ToolUse or Deferred. */
    void done(StopReason reason);
    /** Terminal failure. reason: Error or Aborted. */
    void error(StopReason reason, const std::string& errorMessage);

private:
    void emit(AssistantEventType type, int index, std::string text);
    std::shared_ptr<const AssistantMessage> snapshot() const;

    std::shared_ptr<AssistantMessageStream> m_stream;
    AssistantMessage m_message;
    PartialJsonParser m_json;
    std::map<int, std::string> m_toolJson;
    bool m_finished = false;
};

AssistantStreamEmitter::AssistantStreamEmitter(AssistantMessage initial)
    : m_stream(std::make_shared<AssistantMessageStream>(
          [](const AssistantMessageEvent& event) {
              return event.type == AssistantEventType::Done || event.type == AssistantEventType::Error;
          },
          [](const AssistantMessageEvent& event) { return *event.message; })),
      m_message(std::move(initial)) {}

std::shared_ptr<AssistantMessageStream> AssistantStreamEmitter::stream() const {
    return m_stream;
}

AssistantMessage& AssistantStreamEmitter::message() {
    return m_message;
}

std::shared_ptr<const AssistantMessage> AssistantStreamEmitter::snapshot() const {
    return std::make_shared<const AssistantMessage>(m_message);
}

void AssistantStreamEmitter::emit(AssistantEventType type, int index, std::string text) {
    if (m_finished) {
        return;
    }
    AssistantMessageEvent event;
    event.type = type;
    event.contentIndex = index;
    event.text = std::move(text);
    event.partial = snapshot();
    m_stream->push(std::move(event));
}

void AssistantStreamEmitter::start() {
    emit(AssistantEventType::Start, 0, "");
}

int AssistantStreamEmitter::textStart() {
    m_message.content.emplace_back(TextContent{});
    const int index = static_cast<int>(m_message.content.size()) - 1;
    emit(AssistantEventType::TextStart, index, "");
    return index;
}

void AssistantStreamEmitter::textDelta(int index, const std::string& delta) {
    std::get<TextContent>(m_message.content[static_cast<std::size_t>(index)]).text += delta;
    emit(AssistantEventType::TextDelta, index, delta);
}

void AssistantStreamEmitter::textEnd(int index) {
    emit(AssistantEventType::TextEnd, index,
         std::get<TextContent>(m_message.content[static_cast<std::size_t>(index)]).text);
}

int AssistantStreamEmitter::thinkingStart() {
    m_message.content.emplace_back(ThinkingContent{});
    const int index = static_cast<int>(m_message.content.size()) - 1;
    emit(AssistantEventType::ThinkingStart, index, "");
    return index;
}

void AssistantStreamEmitter::thinkingDelta(int index, const std::string& delta) {
    std::get<ThinkingContent>(m_message.content[static_cast<std::size_t>(index)]).thinking += delta;
    emit(AssistantEventType::ThinkingDelta, index, delta);
}

void AssistantStreamEmitter::thinkingEnd(int index) {
    emit(AssistantEventType::ThinkingEnd, index,
         std::get<ThinkingContent>(m_message.content[static_cast<std::size_t>(index)]).thinking);
}

int AssistantStreamEmitter::toolCallStart(const std::string& id, const std::string& name) {
    ToolCall call;
    call.id = id;
    call.name = name;
    m_message.content.emplace_back(std::move(call));
    const int index = static_cast<int>(m_message.content.size()) - 1;
    m_toolJson[index] = "";
    emit(AssistantEventType::ToolCallStart, index, "");
    return index;
}

void AssistantStreamEmitter::toolCallDelta(int index, const std::string& jsonFragment) {
    std::string& buffer = m_toolJson[index];
    buffer += jsonFragment;
    std::get<ToolCall>(m_message.content[static_cast<std::size_t>(index)]).arguments =
        m_json.parseStreaming(buffer);
    emit(AssistantEventType::ToolCallDelta, index, jsonFragment);
}

void AssistantStreamEmitter::toolCallEnd(int index) {
    ToolCall& call = std::get<ToolCall>(m_message.content[static_cast<std::size_t>(index)]);
    const std::string& buffer = m_toolJson[index];
    if (!buffer.empty()) {
        auto parsed = m_json.parseWithRepair(buffer);
        call.arguments = parsed.has_value() && parsed->is_object() ? *parsed : m_json.parseStreaming(buffer);
    }
    if (m_finished) {
        return;
    }
    AssistantMessageEvent event;
    event.type = AssistantEventType::ToolCallEnd;
    event.contentIndex = index;
    event.partial = snapshot();
    event.toolCall = std::make_shared<const ToolCall>(call);
    m_stream->push(std::move(event));
}

void AssistantStreamEmitter::done(StopReason reason) {
    if (m_finished) {
        return;
    }
    m_message.stopReason = reason;
    AssistantMessageEvent event;
    event.type = AssistantEventType::Done;
    event.reason = reason;
    event.message = snapshot();
    m_stream->push(std::move(event));
    m_finished = true;
}

void AssistantStreamEmitter::error(StopReason reason, const std::string& errorMessage) {
    if (m_finished) {
        return;
    }
    m_message.stopReason = reason;
    m_message.errorMessage = errorMessage;
    AssistantMessageEvent event;
    event.type = AssistantEventType::Error;
    event.reason = reason;
    event.message = snapshot();
    m_stream->push(std::move(event));
    m_finished = true;
}
