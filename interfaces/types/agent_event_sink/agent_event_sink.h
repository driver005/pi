#pragma once

#include <functional>

#include "interfaces/types/agent_event/agent_event.h"

/** Receives agent events; invoked serially (never concurrently) from the running loop. */
using AgentEventSink = std::function<void(const AgentEvent&)>;
