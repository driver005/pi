export module pi.types.cache_warm_request;

import std;
export import pi.types.model;
export import pi.types.stream_options;
export import pi.types.transcript_context;

/** The request whose prompt-cache entry should be kept warm, exactly as it was sent. */
export struct CacheWarmRequest {
    Model model;
    TranscriptContext context;
    StreamOptions options;
};
