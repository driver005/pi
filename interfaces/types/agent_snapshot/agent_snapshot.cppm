export module pi.types.agent_snapshot;

import std;
export import pi.types.json;
export import pi.types.tool_registration;

/** A conversation's resolved agent as a prompt section sees it: model, thinking level and offered tools. */
export struct AgentSnapshot {
    /** `{provider, modelId}` when a model is selected. */
    std::optional<Json> model;
    std::string thinkingLevel = "off";
    std::vector<std::string> extensionNames;
    /** The tools a request offers, in order. */
    std::vector<ToolRegistration> tools;
    std::optional<std::string> instructions;
    std::optional<std::string> cwd;
};
