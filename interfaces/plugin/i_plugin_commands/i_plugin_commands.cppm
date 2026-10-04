export module pi.plugin.i_plugin_commands;

import std;
export import pi.support.abort_signal;
export import pi.types.plugin_command_info;
export import pi.types.plugin_flag;
export import pi.types.result;

/** The commands and flags plugins registered, as the session and the command line see them. */
export class IPluginCommands {
public:
    virtual ~IPluginCommands() = default;

    virtual std::vector<PluginCommandInfo> commands() const = 0;
    /** True when the command exists; its failure is the Result (the prompt that named it is consumed either way). */
    virtual std::optional<Result<void>> execute(const std::string& name, const std::string& args, const std::shared_ptr<AbortSignal>& abort) = 0;

    virtual std::vector<PluginFlag> flags() const = 0;
    /** Sets a flag from the command line: a boolean flag takes no value ("true"/"false" accepted), a string flag needs one. */
    virtual Result<void> setFlag(const std::string& name, const std::optional<std::string>& value) = 0;
};
