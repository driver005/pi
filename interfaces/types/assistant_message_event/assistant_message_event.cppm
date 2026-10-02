export module pi.types.assistant_message_event;

import std;
export import pi.types.assistant_message;
export import pi.types.stop_reason;
export import pi.types.tool_call;

export enum class AssistantEventType {
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
export struct AssistantMessageEvent {
    AssistantEventType type = AssistantEventType::Start;
    int contentIndex = 0;
    /** Text/thinking/toolcall delta, or the full content for *_end events. */
    std::string text;
    std::shared_ptr<const AssistantMessage> partial;
    std::shared_ptr<const ToolCall> toolCall;
    StopReason reason = StopReason::Stop;
    std::shared_ptr<const AssistantMessage> message;
};
