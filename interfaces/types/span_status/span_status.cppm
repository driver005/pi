export module pi.types.span_status;

import std;

/** The outcome of a span: ok, or an error that may carry the error's name and message. */
export struct SpanStatus {
    bool ok = true;
    std::optional<std::string> errorName;
    std::optional<std::string> errorMessage;
};
