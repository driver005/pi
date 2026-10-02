#include "src/ai/faux_provider/faux_provider.h"

#include "interfaces/support/message_codec/message_codec.h"

FauxProvider::FauxProvider(IExecutor& executor, const IClock& clock, std::string api)
    : m_executor(executor), m_clock(clock), m_api(std::move(api)) {}

std::string FauxProvider::api() const {
    return m_api;
}

void FauxProvider::enqueue(AssistantMessage message) {
    enqueue([message = std::move(message)](const TranscriptContext&, const StreamOptions&,
                                          const Model&) { return message; });
}

void FauxProvider::enqueue(ResponseFactory factory) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(factory));
}

int FauxProvider::callCount() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_callCount;
}

AssistantMessage FauxProvider::textResponse(const std::string& text) const {
    AssistantMessage message;
    message.content.emplace_back(TextContent{text, std::nullopt});
    message.stopReason = StopReason::Stop;
    return message;
}

AssistantMessage FauxProvider::toolCallResponse(const std::string& name, const Json& arguments,
                                                const std::string& id) const {
    AssistantMessage message;
    ToolCall call;
    call.id = id;
    call.name = name;
    call.arguments = arguments;
    message.content.emplace_back(std::move(call));
    message.stopReason = StopReason::ToolUse;
    return message;
}

AssistantMessage FauxProvider::nextResponse(const TranscriptContext& context,
                                            const StreamOptions& options, const Model& model) {
    ResponseFactory factory;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_callCount;
        if (!m_queue.empty()) {
            factory = std::move(m_queue.front());
            m_queue.pop_front();
        }
    }
    if (!factory) {
        AssistantMessage message;
        message.stopReason = StopReason::Error;
        message.errorMessage = "No more faux responses queued";
        return message;
    }
    return factory(context, options, model);
}

std::int64_t FauxProvider::estimateTokens(const std::string& text) const {
    return static_cast<std::int64_t>((text.size() + 3) / 4);
}

bool FauxProvider::playText(AssistantStreamEmitter& emitter, const std::string& text, bool thinking,
                            const std::shared_ptr<AbortSignal>& signal) const {
    const int index = thinking ? emitter.thinkingStart() : emitter.textStart();
    constexpr std::size_t kChunk = 4;
    for (std::size_t pos = 0; pos < text.size(); pos += kChunk) {
        if (signal != nullptr && signal->aborted()) {
            return false;
        }
        const std::string chunk = text.substr(pos, kChunk);
        thinking ? emitter.thinkingDelta(index, chunk) : emitter.textDelta(index, chunk);
    }
    thinking ? emitter.thinkingEnd(index) : emitter.textEnd(index);
    return true;
}

void FauxProvider::play(const std::shared_ptr<AssistantStreamEmitter>& emitter,
                        const AssistantMessage& script,
                        const std::shared_ptr<AbortSignal>& signal) const {
    emitter->start();
    for (const AssistantContentBlock& block : script.content) {
        bool completed = true;
        if (const auto* text = std::get_if<TextContent>(&block)) {
            completed = playText(*emitter, text->text, false, signal);
        } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
            completed = playText(*emitter, thinking->thinking, true, signal);
        } else if (const auto* call = std::get_if<ToolCall>(&block)) {
            const int index = emitter->toolCallStart(call->id, call->name);
            emitter->toolCallDelta(index, call->arguments.dump());
            emitter->toolCallEnd(index);
        }
        if (!completed || (signal != nullptr && signal->aborted())) {
            emitter->error(StopReason::Aborted, "Request was aborted");
            return;
        }
    }
    emitter->message().responseId = script.responseId;
    if (script.stopReason == StopReason::Error || script.stopReason == StopReason::Aborted) {
        emitter->error(script.stopReason, script.errorMessage.value_or("faux error"));
        return;
    }
    emitter->message().deferred = script.deferred;
    emitter->done(script.stopReason == StopReason::Pending ? StopReason::Stop : script.stopReason);
}

std::shared_ptr<AssistantMessageStream> FauxProvider::stream(const Model& model,
                                                             const TranscriptContext& context,
                                                             const StreamOptions& options) {
    AssistantMessage initial;
    initial.api = model.api;
    initial.provider = model.provider;
    initial.model = model.id;
    initial.timestamp = m_clock.nowMs();
    auto emitter = std::make_shared<AssistantStreamEmitter>(std::move(initial));
    const AssistantMessage script = nextResponse(context, options, model);
    const MessageCodec codec;
    const std::int64_t inputTokens = estimateTokens(codec.toJson(context.messages).dump());
    std::int64_t outputTokens = 0;
    for (const AssistantContentBlock& block : script.content) {
        outputTokens += estimateTokens(codec.toJson(block).dump());
    }
    emitter->message().usage.input = inputTokens;
    emitter->message().usage.output = outputTokens;
    emitter->message().usage.totalTokens = inputTokens + outputTokens;
    auto stream = emitter->stream();
    m_executor.submit([this, emitter, script, signal = options.signal] {
        play(emitter, script, signal);
    });
    return stream;
}
