export module pi.ai.faux_provider;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_executor;
export import pi.provider.i_provider;
export import pi.support.assistant_stream_emitter;
export import pi.types.faux_deferred_response;
export import pi.types.json;
import pi.support.message_codec;

/**
 * Scripted provider for tests and offline runs: replays queued responses, streaming them in
 * small chunks. An empty queue yields an error response so unexpected extra turns are visible.
 */
export class FauxProvider : public IProvider {
public:
    using ResponseFactory = std::function<AssistantMessage(
        const TranscriptContext&, const StreamOptions&, const Model&)>;

    FauxProvider(IExecutor& executor, const IClock& clock, std::string api = "faux")
        : m_executor(executor),
          m_clock(clock),
          m_api(std::move(api)) {}

    std::string api() const override {
        return m_api;
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        AssistantMessage script = nextResponse(context, options, model);
        script.usage = estimateUsage(context, script);
        if (wantsDeferred(options)) {
            return startDeferred(model, script);
        }
        auto emitter = std::make_shared<AssistantStreamEmitter>(initial(model, script.usage));
        auto stream = emitter->stream();
        m_executor.submit([this, emitter, script, signal = options.signal] { play(emitter, script, signal); });
        return stream;
    }

    bool supportsDeferred() const override {
        return true;
    }

    std::shared_ptr<AssistantMessageStream> fetchDeferred(const Model& model, const DeferredHandle& handle, const StreamOptions& options) override {
        std::optional<FauxDeferredResponse> due;
        std::string failure;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_deferredFetchCount;
            auto found = m_deferred.find(handle.id);
            if (found == m_deferred.end()) {
                failure = "Unknown faux deferred response: " + handle.id;
            } else if (found->second.cancelled) {
                failure = "Faux deferred response was cancelled: " + handle.id;
            } else if (found->second.pendingFetches > 0) {
                --found->second.pendingFetches;
                return pending(model, found->second);
            } else {
                due = found->second;
            }
        }
        if (!due) {
            AssistantStreamEmitter emitter(initial(model, Usage{}));
            emitter.error(StopReason::Error, failure);
            return emitter.stream();
        }
        auto emitter = std::make_shared<AssistantStreamEmitter>(initial(model, due->script.usage));
        auto stream = emitter->stream();
        m_executor.submit([this, emitter, script = due->script, signal = options.signal] { play(emitter, script, signal); });
        return stream;
    }

    Result<void> cancelDeferred(const Model&, const DeferredHandle& handle, const StreamOptions&) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled.push_back(handle);
        auto found = m_deferred.find(handle.id);
        if (found != m_deferred.end()) {
            found->second.cancelled = true;
        }
        return {};
    }

    /** Makes the next deferred responses answer `deferred` to `pendingFetches` fetches, each advising `pollAfterMs`. */
    void setDeferredBehavior(int pendingFetches, std::optional<std::int64_t> pollAfterMs) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingFetches = pendingFetches;
        m_pollAfterMs = pollAfterMs;
    }

    int deferredFetchCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_deferredFetchCount;
    }

    std::vector<DeferredHandle> cancelledDeferred() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelled;
    }

    void enqueue(AssistantMessage message) {
        enqueue([message = std::move(message)](const TranscriptContext&, const StreamOptions&,
                                              const Model&) { return message; });
    }

    void enqueue(ResponseFactory factory) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(factory));
    }

    int callCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_callCount;
    }

    /** Convenience builders for scripted responses. */
    AssistantMessage textResponse(const std::string& text) const {
        AssistantMessage message;
        message.content.emplace_back(TextContent{text, std::nullopt});
        message.stopReason = StopReason::Stop;
        return message;
    }

    AssistantMessage toolCallResponse(const std::string& name, const Json& arguments, const std::string& id) const {
        AssistantMessage message;
        ToolCall call;
        call.id = id;
        call.name = name;
        call.arguments = arguments;
        message.content.emplace_back(std::move(call));
        message.stopReason = StopReason::ToolUse;
        return message;
    }

private:
    AssistantMessage initial(const Model& model, const Usage& usage) const {
        AssistantMessage message;
        message.api = model.api;
        message.provider = model.provider;
        message.model = model.id;
        message.timestamp = m_clock.nowMs();
        message.usage = usage;
        return message;
    }

    Usage estimateUsage(const TranscriptContext& context, const AssistantMessage& script) const {
        const MessageCodec codec;
        Usage usage;
        usage.input = estimateTokens(codec.toJson(context.messages).dump());
        for (const AssistantContentBlock& block : script.content) {
            usage.output += estimateTokens(codec.toJson(block).dump());
        }
        usage.totalTokens = usage.input + usage.output;
        return usage;
    }

    /** The `deferred` stream option is true or a window object. */
    bool wantsDeferred(const StreamOptions& options) const {
        return options.deferred.is_boolean() ? options.deferred.get<bool>() : !options.deferred.is_null();
    }

    /** Registers the script behind a new handle and answers with a `deferred` message. */
    std::shared_ptr<AssistantMessageStream> startDeferred(const Model& model, const AssistantMessage& script) {
        FauxDeferredResponse entry;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            entry.handle.provider = model.provider;
            entry.handle.modelId = model.id;
            entry.handle.api = model.api;
            entry.handle.id = "faux-deferred-" + std::to_string(++m_deferredCount);
            entry.handle.pollAfterMs = m_pollAfterMs;
            entry.script = script;
            entry.pendingFetches = m_pendingFetches;
            m_deferred[entry.handle.id] = entry;
        }
        return pending(model, entry);
    }

    std::shared_ptr<AssistantMessageStream> pending(const Model& model, const FauxDeferredResponse& entry) {
        auto emitter = std::make_shared<AssistantStreamEmitter>(initial(model, Usage{}));
        auto stream = emitter->stream();
        m_executor.submit([emitter, handle = entry.handle] {
            emitter->start();
            emitter->message().deferred = handle;
            emitter->done(StopReason::Deferred);
        });
        return stream;
    }

    AssistantMessage nextResponse(const TranscriptContext& context, const StreamOptions& options, const Model& model) {
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

    void play(const std::shared_ptr<AssistantStreamEmitter>& emitter, const AssistantMessage& script, const std::shared_ptr<AbortSignal>& signal) const {
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

    bool playText(AssistantStreamEmitter& emitter, const std::string& text, bool thinking, const std::shared_ptr<AbortSignal>& signal) const {
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

    std::int64_t estimateTokens(const std::string& text) const {
        return static_cast<std::int64_t>((text.size() + 3) / 4);
    }

    IExecutor& m_executor;
    const IClock& m_clock;
    std::string m_api;
    mutable std::mutex m_mutex;
    std::deque<ResponseFactory> m_queue;
    int m_callCount = 0;
    int m_pendingFetches = 0;
    std::optional<std::int64_t> m_pollAfterMs;
    int m_deferredCount = 0;
    int m_deferredFetchCount = 0;
    std::map<std::string, FauxDeferredResponse> m_deferred;
    std::vector<DeferredHandle> m_cancelled;
};
