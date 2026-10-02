#pragma once

#include <vector>

#include "interfaces/types/message/message.h"

/** Normalized context handed to providers: prompt and tools live in leading system messages. */
struct TranscriptContext {
    std::vector<Message> messages;
};
