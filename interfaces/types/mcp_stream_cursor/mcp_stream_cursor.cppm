export module pi.types.mcp_stream_cursor;

import std;
export import pi.types.json;

/** Where a message stream stands, so a dropped one can be resumed with Last-Event-ID. */
export struct McpStreamCursor {
    std::string lastEventId;
    std::optional<int> retryMs;
    /** Whether the stream delivered anything since it was (re)opened. */
    bool received = false;
    /** Id of the request this stream is expected to answer; null for the server-to-client stream. */
    Json awaitedId;
    bool answered = false;
};
