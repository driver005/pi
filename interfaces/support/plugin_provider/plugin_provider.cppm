module;

#include <cstdint>

#include "pi_plugin.h"

export module pi.support.plugin_provider;

import std;
export import pi.platform.i_clock;
export import pi.provider.i_provider;
export import pi.support.message_codec;
export import pi.support.model_codec;
export import pi.support.plugin_stream_runs;
export import pi.support.plugin_stream_translator;
export import pi.support.thinking_level_resolver;

/**
 * The wire API of a stream-handler provider of a plugin (register_stream_provider): every request runs the plugin's PiStreamFn
 * on a thread of its own, which reports the response through a PluginStreamTranslator. The plugin's code and data must stay
 * mapped while a stream runs, so shutdown() cancels the running streams and waits for their functions to return; after it the
 * provider answers requests with an error. The bookkeeping is shared with the threads, so dropping the provider while a stream
 * runs is safe too (only unloading the plugin is not: call shutdown() first).
 */
export class PluginProvider : public IProvider {
public:
    PluginProvider(std::string api, PiStreamFn stream, void* userData, const IClock& clock)
        : m_api(std::move(api)),
          m_clock(clock),
          m_function(stream),
          m_userData(userData),
          m_runs(std::make_shared<PluginStreamRuns>()) {}

    std::string api() const override {
        return m_api;
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model& model, const TranscriptContext& context, const StreamOptions& options) override {
        auto abort = std::make_shared<AbortSignal>();
        const std::uint64_t link = options.signal ? options.signal->onAbort([abort] { abort->abort(); }) : 0;
        auto translator = std::make_shared<PluginStreamTranslator>(model, abort, m_clock.nowMs());
        std::string request = requestJson(model, context, options).dump(-1, ' ', false, Json::error_handler_t::replace);
        if (!m_runs->begin(abort)) {
            translator->emit(R"({"type":"error","message":"the plugin that provides this API was unloaded"})");
            return translator->stream();
        }
        std::thread([runs = m_runs, function = m_function, userData = m_userData, translator, abort, signal = options.signal, link, request = std::move(request)] {
            function(userData, PiString{request.data(), request.size()}, reinterpret_cast<const PiAbort*>(abort.get()), reinterpret_cast<PiStreamSink*>(translator.get()));
            translator->finish();
            if (signal) {
                signal->removeListener(link);
            }
            runs->end(abort);
        }).detach();
        return translator->stream();
    }

    /** Cancels the running streams, waits for them to end and refuses new ones. */
    void shutdown() {
        m_runs->close();
    }

private:
    Json requestJson(const Model& model, const TranscriptContext& context, const StreamOptions& options) const {
        Json headers = Json::object();
        for (const auto& [name, value] : options.headers) {
            if (value) {
                headers[name] = *value;
            }
        }
        Json out = Json::object({{"reasoning", m_levels.levelName(options.reasoning)}, {"headers", std::move(headers)}});
        if (options.apiKey) {
            out["apiKey"] = *options.apiKey;
        }
        if (options.temperature) {
            out["temperature"] = *options.temperature;
        }
        if (options.maxTokens) {
            out["maxTokens"] = *options.maxTokens;
        }
        if (options.sessionId) {
            out["sessionId"] = *options.sessionId;
        }
        if (options.toolChoice) {
            out["toolChoice"] = *options.toolChoice;
        }
        if (options.cacheRetention) {
            out["cacheRetention"] = *options.cacheRetention;
        }
        if (options.timeoutMs) {
            out["timeoutMs"] = *options.timeoutMs;
        }
        if (!options.metadata.is_null()) {
            out["metadata"] = options.metadata;
        }
        if (!options.samplingParams.is_null()) {
            out["samplingParams"] = options.samplingParams;
        }
        return Json::object({{"model", m_modelCodec.toJson(model)}, {"messages", m_messageCodec.toJson(context.messages)}, {"options", std::move(out)}});
    }

    std::string m_api;
    const IClock& m_clock;
    PiStreamFn m_function;
    void* m_userData;
    std::shared_ptr<PluginStreamRuns> m_runs;
    ModelCodec m_modelCodec;
    MessageCodec m_messageCodec;
    ThinkingLevelResolver m_levels;
};
