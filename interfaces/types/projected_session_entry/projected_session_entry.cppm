export module pi.types.projected_session_entry;

import std;
export import pi.types.agent_message;
export import pi.types.session_entry;

/** An entry that is part of the model context and the messages it contributes after edits. */
export struct ProjectedSessionEntry {
    SessionEntry sourceEntry;
    std::vector<AgentMessage> messages;
};
