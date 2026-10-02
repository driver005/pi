export module pi.types.session_projection;

import std;
export import pi.types.projected_session_entry;
export import pi.types.session_model_ref;

/** Provenance-preserving model context: every contributing entry with its messages. */
export struct SessionProjection {
    std::vector<ProjectedSessionEntry> entries;
    std::vector<AgentMessage> messages;
    std::string thinkingLevel = "off";
    std::optional<SessionModelRef> model;
};
