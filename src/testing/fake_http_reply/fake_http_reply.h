#pragma once

#include <string>
#include <vector>

#include "interfaces/types/http_headers/http_headers.h"

/** What FakeHttpServer answers. Chunks are written one by one with a pause between them. */
struct FakeHttpReply {
    int status = 200;
    HttpHeaders headers;
    std::vector<std::string> chunks;
    int chunkDelayMs = 0;
    /** Pause before the status line, to provoke client timeouts. */
    int initialDelayMs = 0;
    /** Keep the connection open (silent) this long after the last chunk. */
    int tailDelayMs = 0;
};
