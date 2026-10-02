export module pi.support.command_line_parser;

import std;
export import pi.platform.i_environment;
export import pi.types.command_line;
export import pi.types.result;

/**
 * Parses `pi <command> [flags]`. The agent directory defaults to $PI_CODING_AGENT_DIR, else
 * ~/.pi/agent; "~" and "~/" at the start of a directory flag expand to $HOME.
 */
export class CommandLineParser {
public:
    explicit CommandLineParser(const IEnvironment& environment);

    /** args excludes the program name; currentDirectory is the default cwd. */
    Result<CommandLine> parse(const std::vector<std::string>& args, const std::string& currentDirectory) const;

    std::string usage() const;

private:
    Result<void> applySwitch(const std::string& flag, CommandLine& line) const;
    Result<void> applyValueFlag(const std::string& flag, const std::string& value, CommandLine& line) const;
    Result<void> applyMcpWait(const std::string& value, CodingStartupOptions& startup) const;
    std::string expandHome(const std::string& path) const;
    std::string defaultAgentDir() const;
    std::vector<std::string> splitList(const std::string& text) const;
    Error missingValue(const std::string& flag) const;

    const IEnvironment& m_environment;
    const std::set<std::string> m_valueFlags{"--cwd",           "--agent-dir",           "--catalog-dir",
                                             "--model",         "--thinking",            "--session",
                                             "--session-dir",   "--tools",               "--system-prompt",
                                             "--append-system-prompt", "--skill",        "--prompt-template",
                                             "--mcp-wait"};
};

CommandLineParser::CommandLineParser(const IEnvironment& environment) : m_environment(environment) {}

std::string CommandLineParser::usage() const {
    return "Usage: pi rpc [options]\n"
           "\n"
           "Serves JSONL commands on stdin and writes responses and events to stdout.\n"
           "\n"
           "Options:\n"
           "  --cwd <dir>                   Working directory (default: current directory)\n"
           "  --agent-dir <dir>             Agent directory (default: $PI_CODING_AGENT_DIR or ~/.pi/agent)\n"
           "  --catalog-dir <dir>           Model catalog directory (default: <agent-dir>/catalog)\n"
           "  --model <provider/id[:lvl]>   Starting model, optionally with a thinking level\n"
           "  --thinking <level>            Starting thinking level\n"
           "  --continue, -c                Continue the most recent session of the cwd\n"
           "  --session <path|id>           Open a session file or id\n"
           "  --session-dir <dir>           Directory for session files\n"
           "  --no-session                  Do not write the session to disk\n"
           "  --tools <a,b,...>             Active tools\n"
           "  --no-tools                    Start with no active tools\n"
           "  --system-prompt <text|file>   Replace the system prompt\n"
           "  --append-system-prompt <text|file>  Append to the system prompt (repeatable)\n"
           "  --no-context-files            Do not load AGENTS.md / CLAUDE.md\n"
           "  --no-skills                   Do not load skills\n"
           "  --no-prompt-templates         Do not load prompt templates\n"
           "  --skill <path>                Extra skill file or directory (repeatable)\n"
           "  --prompt-template <path>      Extra prompt template (repeatable)\n"
           "  --no-mcp                      Do not connect MCP servers\n"
           "  --mcp-wait <ms>               How long startup waits for MCP servers (default 5000)\n"
           "  --trust / --no-trust          Answer the project trust question\n"
           "  --faux                        Add the scripted offline provider (PI_FAUX_REPLIES)\n"
           "  --help, -h                    Show this help\n";
}

Error CommandLineParser::missingValue(const std::string& flag) const {
    return Error{"usage", "Missing value for " + flag};
}

std::string CommandLineParser::expandHome(const std::string& path) const {
    const auto home = m_environment.get("HOME");
    if (!home || path.empty() || path[0] != '~' || (path.size() > 1 && path[1] != '/')) {
        return path;
    }
    return *home + path.substr(1);
}

std::string CommandLineParser::defaultAgentDir() const {
    if (const auto configured = m_environment.get("PI_CODING_AGENT_DIR"); configured && !configured->empty()) {
        return expandHome(*configured);
    }
    return m_environment.get("HOME").value_or("") + "/.pi/agent";
}

std::vector<std::string> CommandLineParser::splitList(const std::string& text) const {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

Result<CommandLine> CommandLineParser::parse(const std::vector<std::string>& args,
                                             const std::string& currentDirectory) const {
    CommandLine line;
    line.options.cwd = currentDirectory;
    line.options.agentDir = defaultAgentDir();
    std::size_t index = 0;
    if (index < args.size() && !args[index].starts_with("-")) {
        line.command = args[index++];
    } else {
        line.command = "rpc";
    }
    while (index < args.size()) {
        const std::string flag = args[index++];
        Result<void> applied;
        if (!m_valueFlags.contains(flag)) {
            applied = applySwitch(flag, line);
        } else if (index >= args.size()) {
            applied = std::unexpected(missingValue(flag));
        } else {
            applied = applyValueFlag(flag, args[index++], line);
        }
        if (!applied) {
            return std::unexpected(applied.error());
        }
    }
    if (line.help) {
        line.command.clear();
    } else if (line.command != "rpc") {
        return std::unexpected(Error{"usage", "Unknown command \"" + line.command + "\""});
    }
    return line;
}

Result<void> CommandLineParser::applySwitch(const std::string& flag, CommandLine& line) const {
    CodingApplicationOptions& options = line.options;
    if (flag == "--help" || flag == "-h") {
        line.help = true;
    } else if (flag == "--continue" || flag == "-c") {
        options.sessionMode = SessionStartMode::Continue;
    } else if (flag == "--no-session") {
        options.sessionMode = SessionStartMode::InMemory;
    } else if (flag == "--no-tools") {
        options.startup.noTools = true;
    } else if (flag == "--no-context-files") {
        options.startup.noContextFiles = true;
    } else if (flag == "--no-skills") {
        options.startup.noSkills = true;
    } else if (flag == "--no-prompt-templates") {
        options.startup.noPromptTemplates = true;
    } else if (flag == "--no-mcp") {
        options.startup.noMcp = true;
    } else if (flag == "--trust") {
        options.startup.trustProject = true;
    } else if (flag == "--no-trust") {
        options.startup.trustProject = false;
    } else if (flag == "--faux") {
        options.faux = true;
    } else {
        return std::unexpected(Error{"usage", "Unknown option " + flag});
    }
    return {};
}

Result<void> CommandLineParser::applyMcpWait(const std::string& value,
                                             CodingStartupOptions& startup) const {
    std::int64_t milliseconds = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
    if (error != std::errc() || end != value.data() + value.size() || milliseconds < 0) {
        return std::unexpected(Error{"usage", "--mcp-wait expects a number of milliseconds"});
    }
    startup.mcpStartupWaitMs = milliseconds;
    return {};
}

Result<void> CommandLineParser::applyValueFlag(const std::string& flag, const std::string& value,
                                               CommandLine& line) const {
    CodingApplicationOptions& options = line.options;
    if (flag == "--cwd") {
        options.cwd = expandHome(value);
    } else if (flag == "--agent-dir") {
        options.agentDir = expandHome(value);
    } else if (flag == "--catalog-dir") {
        options.catalogDir = expandHome(value);
    } else if (flag == "--model") {
        options.startup.model = value;
    } else if (flag == "--thinking") {
        options.startup.thinking = value;
    } else if (flag == "--session") {
        options.sessionMode = SessionStartMode::Open;
        options.sessionRef = value;
    } else if (flag == "--session-dir") {
        options.sessionDir = expandHome(value);
    } else if (flag == "--tools") {
        options.startup.tools = splitList(value);
    } else if (flag == "--system-prompt") {
        options.startup.systemPrompt = value;
    } else if (flag == "--append-system-prompt") {
        if (!options.startup.appendSystemPrompt) {
            options.startup.appendSystemPrompt = std::vector<std::string>{};
        }
        options.startup.appendSystemPrompt->push_back(value);
    } else if (flag == "--mcp-wait") {
        return applyMcpWait(value, options.startup);
    } else if (flag == "--skill") {
        options.startup.skillPaths.push_back(expandHome(value));
    } else {
        options.startup.promptTemplatePaths.push_back(expandHome(value));
    }
    return {};
}
