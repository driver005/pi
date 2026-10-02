#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "interfaces/platform/i_clock/i_clock.h"
#include "interfaces/platform/i_executor/i_executor.h"
#include "interfaces/provider/i_provider/i_provider.h"
#include "interfaces/support/assistant_stream_emitter/assistant_stream_emitter.h"
#include "interfaces/types/json/json.h"

/**
 * Scripted provider for tests and offline runs: replays queued responses, streaming them in
 * small chunks. An empty queue yields an error response so unexpected extra turns are visible.
 */
class FauxProvider : public IProvider {
public:
    using ResponseFactory = std::function<AssistantMessage(
        const TranscriptContext&, const StreamOptions&, const Model&)>;

    FauxProvider(IExecutor& executor, const IClock& clock, std::string api = "faux");

    std::string api() const override;
    std::shared_ptr<AssistantMessageStream> stream(const Model& model,
                                                   const TranscriptContext& context,
                                                   const StreamOptions& options) override;

    void enqueue(AssistantMessage message);
    void enqueue(ResponseFactory factory);
    int callCount() const;

    /** Convenience builders for scripted responses. */
    AssistantMessage textResponse(const std::string& text) const;
    AssistantMessage toolCallResponse(const std::string& name, const Json& arguments,
                                      const std::string& id) const;

private:
    AssistantMessage nextResponse(const TranscriptContext& context, const StreamOptions& options,
                                  const Model& model);
    void play(const std::shared_ptr<AssistantStreamEmitter>& emitter, const AssistantMessage& script,
              const std::shared_ptr<AbortSignal>& signal) const;
    bool playText(AssistantStreamEmitter& emitter, const std::string& text, bool thinking,
                  const std::shared_ptr<AbortSignal>& signal) const;
    std::int64_t estimateTokens(const std::string& text) const;

    IExecutor& m_executor;
    const IClock& m_clock;
    std::string m_api;
    mutable std::mutex m_mutex;
    std::deque<ResponseFactory> m_queue;
    int m_callCount = 0;
};
