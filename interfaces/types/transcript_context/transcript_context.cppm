export module pi.types.transcript_context;

import std;
export import pi.types.message;

/** Normalized context handed to providers: prompt and tools live in leading system messages. */
export struct TranscriptContext {
    std::vector<Message> messages;
};
