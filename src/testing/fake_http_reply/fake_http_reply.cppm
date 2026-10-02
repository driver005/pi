export module pi.testing.fake_http_reply;

import std;
export import pi.types.http_headers;

/** What FakeHttpServer answers. Chunks are written one by one with a pause between them. */
export struct FakeHttpReply {
    int status = 200;
    HttpHeaders headers;
    std::vector<std::string> chunks;
    int chunkDelayMs = 0;
    /** Pause before the status line, to provoke client timeouts. */
    int initialDelayMs = 0;
    /** Keep the connection open (silent) this long after the last chunk. */
    int tailDelayMs = 0;
};
