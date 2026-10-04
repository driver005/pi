export module pi.types.bug_report_input;

import std;
export import pi.types.json;
export import pi.types.model;

/** What BugReportBuilder describes: the session, its model and provider, the loaded plugins and the settings. */
export struct BugReportInput {
    /** The report id; a fresh UUIDv7 when absent. */
    std::optional<std::string> id;
    std::optional<std::string> hint;
    std::string sessionId;
    std::string cwd;
    bool includeSession = false;
    bool includeSummary = false;
    int messageCount = 0;
    std::optional<Model> model;
    /** From BugReportBuilder::describeProvider; null when there is no model. */
    Json provider;
    std::string thinkingLevel = "off";
    /** Paths of the loaded plugins. */
    std::vector<std::string> plugins;
    /** Plugins that failed to load: {path, error}. */
    std::vector<std::pair<std::string, std::string>> pluginErrors;
    Json globalSettings = Json::object();
    Json projectSettings = Json::object();
};
