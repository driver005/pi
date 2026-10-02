#pragma once

#include <map>
#include <memory>
#include <string>

#include "interfaces/provider/i_provider/i_provider.h"
#include "interfaces/support/partial_json_parser/partial_json_parser.h"
#include "interfaces/types/assistant_message/assistant_message.h"

/**
 * Builds an AssistantMessage block by block and publishes the matching stream events with a
 * snapshot of the response so far. Every provider drives one of these from its own thread.
 * Block indices returned by the *Start methods are positions in message().content.
 */
class AssistantStreamEmitter {
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
