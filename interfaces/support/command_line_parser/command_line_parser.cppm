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
    explicit CommandLineParser(const IEnvironment& environment)
        : m_environment(environment) {}

    /** args excludes the program name; currentDirectory is the default cwd. */
    Result<CommandLine> parse(const std::vector<std::string>& args, const std::string& currentDirectory) const {
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
            if (!flag.starts_with("-")) {
                if (!takesArguments(line.command)) {
                    return std::unexpected(Error{"usage", "Unexpected argument \"" + flag + "\""});
                }
                line.arguments.push_back(flag);
                continue;
            }
            if (isPluginFlag(flag, line.command)) {
                collectPluginFlag(flag, args, index, line.options.startup);
                continue;
            }
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
        } else if (line.command != "rpc" && line.command != "serve" && !takesArguments(line.command)) {
            return std::unexpected(Error{"usage", "Unknown command \"" + line.command + "\""});
        }
        if (const auto valid = validatePackages(line); !valid) {
            return std::unexpected(valid.error());
        }
        if (line.command == "mcp") {
            if (auto valid = validateMcp(line.arguments); !valid) {
                return std::unexpected(valid.error());
            }
        }
        if (line.command == "export" && (line.arguments.empty() || line.arguments.size() > 2)) {
            return std::unexpected(Error{"usage", "Usage: pi export <session.jsonl> [output.html] [--theme dark|light]"});
        }
        if (line.command == "auth") {
            if (auto valid = validateAuth(line.arguments); !valid) {
                return std::unexpected(valid.error());
            }
        }
        if (line.command == "serve") {
            resolveServe(line);
        }
        return line;
    }

    std::string usage() const {
        return "Usage: pi rpc|serve|mcp|auth|export|install|remove|update|list [options]\n"
               "\n"
               "rpc    Serves JSONL commands on stdin and writes responses and events to stdout.\n"
               "serve  Serves the Pi protocol (CBOR) on a unix socket in the server directory.\n"
               "mcp    pi mcp list | login <server> | logout <server>: MCP server sign-in (OAuth).\n"
               "auth   pi auth list | status | login <provider> | logout <provider>: sign in to subscription providers.\n"
               "export  pi export <session.jsonl> [output.html]: write a session as a self-contained HTML page (--theme dark|light).\n"
               "install, remove, update, list   pi install|remove <source> [-l], pi update [source], pi list: manage packages\n"
               "                                (skills, prompt templates and plugins from git repositories and local directories).\n"
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
               "  --plugin <path>               Load a plugin library (repeatable)\n"
               "  --no-plugins                  Do not load plugins\n"
               "  --no-mcp                      Do not connect MCP servers\n"
               "  --mcp-wait <ms>               How long startup waits for MCP servers (default 5000)\n"
               "  --trust / --no-trust          Answer the project trust question\n"
               "  --faux                        Add the scripted offline provider (PI_FAUX_REPLIES)\n"
               "  --theme <name>                export: color theme of the page (dark or light, default dark)\n"
               "  --method <id>                 auth login: sign-in method (browser, copy_code, device_code)\n"
               "  --manual                      auth login: paste the code instead of using the local callback\n"
               "  --local, -l                   install, remove: use the project settings instead of the global ones\n"
               "  --server-dir <dir>            serve: profile and socket directory (default: $PI_SERVER_DIR or ~/.pi/server)\n"
               "  --server-id <uuid>            serve: logical server id (default: $PI_SERVER_ID or the directory's default)\n"
               "  --session-tree                serve: keep sessions as session trees instead of durable (SQLite) sessions\n"
               "                                serve keeps its sessions in --session-dir (default: <agent-dir>/server-sessions)\n"
               "  --<flag> [value]              rpc, serve: a flag a plugin declares (see its documentation)\n"
               "  --help, -h                    Show this help\n";
    }

private:
    /** A `--name` or `--name=value` the parser does not know, on a command that loads plugins (which may declare it). */
    bool isPluginFlag(const std::string& flag, const std::string& command) const {
        if (!flag.starts_with("--") || flag.size() < 3 || (command != "rpc" && command != "serve")) {
            return false;
        }
        const std::string name = flag.substr(0, flag.find('='));
        return !m_valueFlags.contains(name) && !m_switches.contains(name);
    }

    /** `--name=value`, or `--name value` when the next argument is not a flag, or `--name` alone. */
    void collectPluginFlag(const std::string& flag, const std::vector<std::string>& args, std::size_t& index, CodingStartupOptions& startup) const {
        const std::size_t equals = flag.find('=');
        if (equals != std::string::npos) {
            startup.pluginFlags[flag.substr(2, equals - 2)] = flag.substr(equals + 1);
        } else if (index < args.size() && !args[index].starts_with("-")) {
            startup.pluginFlags[flag.substr(2)] = args[index++];
        } else {
            startup.pluginFlags[flag.substr(2)] = std::nullopt;
        }
    }

    Result<void> applySwitch(const std::string& flag, CommandLine& line) const {
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
        } else if (flag == "--no-plugins") {
            options.startup.noPlugins = true;
        } else if (flag == "--no-mcp") {
            options.startup.noMcp = true;
        } else if (flag == "--trust") {
            options.startup.trustProject = true;
        } else if (flag == "--no-trust") {
            options.startup.trustProject = false;
        } else if (flag == "--faux") {
            options.faux = true;
        } else if (flag == "--session-tree") {
            line.sessionTree = true;
        } else if (flag == "--manual") {
            line.loginManual = true;
        } else if (flag == "--local" || flag == "-l") {
            line.localPackages = true;
        } else {
            return std::unexpected(Error{"usage", "Unknown option " + flag});
        }
        return {};
    }

    Result<void> applyValueFlag(const std::string& flag, const std::string& value, CommandLine& line) const {
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
        } else if (flag == "--server-dir") {
            line.serverDir = expandHome(value);
        } else if (flag == "--server-id") {
            line.serverId = value;
        } else if (flag == "--method") {
            line.loginMethod = value;
        } else if (flag == "--theme") {
            line.exportTheme = value;
        } else if (flag == "--plugin") {
            options.startup.pluginPaths.push_back(expandHome(value));
        } else if (flag == "--mcp-wait") {
            return applyMcpWait(value, options.startup);
        } else if (flag == "--skill") {
            options.startup.skillPaths.push_back(expandHome(value));
        } else {
            options.startup.promptTemplatePaths.push_back(expandHome(value));
        }
        return {};
    }

    Result<void> applyMcpWait(const std::string& value, CodingStartupOptions& startup) const {
        std::int64_t milliseconds = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
        if (error != std::errc() || end != value.data() + value.size() || milliseconds < 0) {
            return std::unexpected(Error{"usage", "--mcp-wait expects a number of milliseconds"});
        }
        startup.mcpStartupWaitMs = milliseconds;
        return {};
    }

    bool takesArguments(const std::string& command) const {
        return command == "mcp" || command == "auth" || command == "export" || command == "install" || command == "remove" || command == "update" || command == "list";
    }

    Result<void> validatePackages(const CommandLine& line) const {
        const std::string& command = line.command;
        if (command != "install" && command != "remove" && command != "update" && command != "list") {
            return {};
        }
        const std::size_t count = line.arguments.size();
        const bool valid = command == "update" ? count <= 1 : (command == "list" ? count == 0 : count == 1);
        if (!valid) {
            return std::unexpected(Error{"usage", "Usage: pi install <source> [-l] | pi remove <source> [-l] | pi update [source] | pi list"});
        }
        return {};
    }

    Result<void> validateAuth(const std::vector<std::string>& arguments) const {
        const std::string usage = "Usage: pi auth list | status | login <provider> | logout <provider>";
        if (arguments.empty() || (arguments[0] != "list" && arguments[0] != "status" && arguments[0] != "login" && arguments[0] != "logout")) {
            return std::unexpected(Error{"usage", usage});
        }
        const bool takesProvider = arguments[0] == "login" || arguments[0] == "logout";
        if (arguments.size() != (takesProvider ? 2U : 1U)) {
            return std::unexpected(Error{"usage", usage});
        }
        return {};
    }

    Result<void> validateMcp(const std::vector<std::string>& arguments) const {
        const std::string usage = "Usage: pi mcp list | login <server> | logout <server>";
        if (arguments.empty() || (arguments[0] != "list" && arguments[0] != "login" && arguments[0] != "logout")) {
            return std::unexpected(Error{"usage", usage});
        }
        if (arguments.size() != (arguments[0] == "list" ? 1U : 2U)) {
            return std::unexpected(Error{"usage", usage});
        }
        return {};
    }

    void resolveServe(CommandLine& line) const {
        if (line.serverDir.empty()) {
            const auto configured = m_environment.get("PI_SERVER_DIR");
            line.serverDir = configured && !configured->empty()
                                 ? expandHome(*configured)
                                 : m_environment.get("HOME").value_or("") + "/.pi/server";
        }
        if (!line.serverId) {
            const auto configured = m_environment.get("PI_SERVER_ID");
            if (configured && !configured->empty()) {
                line.serverId = *configured;
            }
        }
        if (!line.options.sessionDir) {
            line.options.sessionDir = line.options.agentDir + "/server-sessions";
        }
    }

    std::string expandHome(const std::string& path) const {
        const auto home = m_environment.get("HOME");
        if (!home || path.empty() || path[0] != '~' || (path.size() > 1 && path[1] != '/')) {
            return path;
        }
        return *home + path.substr(1);
    }

    std::string defaultAgentDir() const {
        if (const auto configured = m_environment.get("PI_CODING_AGENT_DIR"); configured && !configured->empty()) {
            return expandHome(*configured);
        }
        return m_environment.get("HOME").value_or("") + "/.pi/agent";
    }

    std::vector<std::string> splitList(const std::string& text) const {
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

    Error missingValue(const std::string& flag) const {
        return Error{"usage", "Missing value for " + flag};
    }

    const IEnvironment& m_environment;
    const std::set<std::string> m_valueFlags{"--cwd",           "--agent-dir",           "--catalog-dir",
                                             "--model",         "--thinking",            "--session",
                                             "--session-dir",   "--tools",               "--system-prompt",
                                             "--append-system-prompt", "--skill",        "--prompt-template",
                                             "--mcp-wait", "--plugin", "--server-dir", "--server-id", "--method", "--theme"};
    const std::set<std::string> m_switches{"--help", "-h", "--continue", "-c", "--no-session", "--no-tools", "--no-context-files",
                                           "--no-skills", "--no-prompt-templates", "--no-plugins", "--no-mcp", "--trust",
                                           "--no-trust", "--faux", "--session-tree", "--manual", "--local", "-l"};
};
