module;

#include <cstdint>

export module pi.types.coding_startup_options;

import std;

/** Command-line choices that shape every session the process starts. */
export struct CodingStartupOptions {
    /** "provider/id" or a bare model id, optionally ending in ":thinking-level". */
    std::optional<std::string> model;
    std::optional<std::string> thinking;
    /** Names of the tools to activate; nullopt uses the defaultTools setting or the built-in set. */
    std::optional<std::vector<std::string>> tools;
    bool noTools = false;
    /** Replaces the discovered SYSTEM.md: a file path or literal text. */
    std::optional<std::string> systemPrompt;
    std::optional<std::vector<std::string>> appendSystemPrompt;
    bool noContextFiles = false;
    bool noSkills = false;
    bool noPromptTemplates = false;
    std::vector<std::string> skillPaths;
    std::vector<std::string> promptTemplatePaths;
    /** Explicit answer to "trust this project?"; nullopt resolves it from the trust store. */
    std::optional<bool> trustProject;
    /** Do not connect the MCP servers of mcp.json. */
    bool noMcp = false;
    /** How long startup waits for MCP servers to connect; slower ones add their tools when ready. */
    std::int64_t mcpStartupWaitMs = 5000;
};
