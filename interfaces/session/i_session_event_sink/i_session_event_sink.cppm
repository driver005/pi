export module pi.session.i_session_event_sink;

import std;
export import pi.types.agent_session_event;

/** Receives session events; implementations deliver them to subscribers in order. */
export class ISessionEventSink {
public:
    virtual ~ISessionEventSink() = default;

    virtual void emit(const AgentSessionEvent& event) = 0;
};
