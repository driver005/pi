export module pi.types.aws_event_stream_message;

import std;

/** One application/vnd.amazon.eventstream message: its string headers and raw payload. */
export struct AwsEventStreamMessage {
    /** String-typed headers such as ":message-type", ":event-type" and ":exception-type". */
    std::map<std::string, std::string> headers;
    std::string payload;
};
