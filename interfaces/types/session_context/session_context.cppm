export module pi.types.session_context;

import std;
export import pi.types.agent_message;
export import pi.types.session_model_ref;

/** What a session contributes to the next model request. */
export struct SessionContext {
    std::vector<AgentMessage> messages;
    std::string thinkingLevel = "off";
    std::optional<SessionModelRef> model;
};
