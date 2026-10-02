export module pi.types.agent_event_sink;

import std;
export import pi.types.agent_event;

/** Receives agent events; invoked serially (never concurrently) from the running loop. */
export using AgentEventSink = std::function<void(const AgentEvent&)>;
