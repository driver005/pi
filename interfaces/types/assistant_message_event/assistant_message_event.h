#pragma once

#include <memory>
#include <string>

#include "interfaces/types/assistant_message/assistant_message.h"
#include "interfaces/types/stop_reason/stop_reason.h"
#include "interfaces/types/tool_call/tool_call.h"

enum class AssistantEventType {
    Start,
    TextStart,
    TextDelta,
    TextEnd,
    ThinkingStart,
    ThinkingDelta,
    ThinkingEnd,
    ToolCallStart,
    ToolCallDelta,
    ToolCallEnd,
    Done,
    Error
};

/**
 * Stream event. Successful streams emit Start, then updates, then Done; failures end with
 * Error. `partial` is a snapshot of the response so far (not shared with the producer).
 * Done: `message` is final and `reason` is stop|length|toolUse|deferred.
 * Error: `message` is the failed message and `reason` is error|aborted.
 */
struct AssistantMessageEvent {
    AssistantEventType type = AssistantEventType::Start;
    int contentIndex = 0;
    /** Text/thinking/toolcall delta, or the full content for *_end events. */
    std::string text;
    std::shared_ptr<const AssistantMessage> partial;
    std::shared_ptr<const ToolCall> toolCall;
    StopReason reason = StopReason::Stop;
    std::shared_ptr<const AssistantMessage> message;
};
