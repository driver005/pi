export module pi.types.resolved_settings;

import std;
export import pi.types.conversation_compaction_policy;
export import pi.types.conversation_retry_policy;
export import pi.types.json;

/** The run policy with every field over its default. */
export struct ResolvedSettings {
    std::optional<std::vector<std::string>> extensions;
    Json stream = Json::object();
    ConversationRetryPolicy retry;
    ConversationCompactionPolicy compaction;
    std::string toolExecution = "parallel";
    std::string steeringMode = "one-at-a-time";
    std::string followUpMode = "one-at-a-time";
};
