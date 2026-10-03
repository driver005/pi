export module pi.types.mcp_server_config;

import std;
export import pi.types.json;
export import pi.types.mcp_exposure;

/** One validated entry of an mcp.json `mcpServers` object. */
export struct McpServerConfig {
    std::string name;
    /** Streamable HTTP when true, stdio otherwise. */
    bool http = false;

    std::string command;
    std::vector<std::string> args;
    /** Values may reference environment variables (`${NAME}`) or commands (`!cmd`). */
    std::map<std::string, std::string> env;
    std::optional<std::string> cwd;

    std::string url;
    /** Values may reference environment variables (`${NAME}`) or commands (`!cmd`). */
    std::map<std::string, std::string> headers;
    /** Name of a pi provider whose token is sent as the bearer token. */
    std::optional<std::string> authProvider;

    McpExposure exposure = McpExposure::Codemode;
    std::optional<std::string> description;
    /** Per-tool exposure, in file order: exact names or patterns where `*` matches anything. */
    std::vector<std::pair<std::string, McpExposure>> toolExposure;
    bool enabled = true;
    /** Per-request timeout; nullopt means the default of 60 seconds. */
    std::optional<double> timeoutSeconds;

    /** The config file that defined the entry. */
    std::string source;
    /** "global" or "project". */
    std::string scope;
    /** Project mcp.json with an override of this global server's enabled/exposure/toolExposure. */
    std::optional<std::string> overrideSource;
    /** The entry as written (exposure aliases resolved), kept to merge project overrides into. */
    Json raw;
};
