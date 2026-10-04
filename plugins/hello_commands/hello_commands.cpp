// style:c-abi
// An example plugin: registers the commands `/note <text>` and `/ping` and the flags `--note-prefix <text>` and `--loud`.
// `/note` appends a "hello-note" entry to the session (the text gets the prefix, upper-cased with --loud); `/ping` emits an
// event on the plugin event bus that this plugin also listens to, which appends a "hello-ping" entry.
#include <algorithm>
#include <cctype>

#include "pi_plugin.hpp"

class HelloCommandsPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        m_host = &host;
        const std::string errors = host.registerFlag("note-prefix", pi::Json{{"description", "Prefix of /note entries"}, {"type", "string"}, {"default", "note: "}}) +
                                   host.registerFlag("loud", pi::Json{{"description", "Upper-case /note entries"}, {"type", "boolean"}}) +
                                   host.registerCommand("note", pi::Json{{"description", "Append a note to the session"}}, [this](const pi::CommandCall& call) { return note(call); }) +
                                   host.registerCommand("ping", pi::Json{{"description", "Emit a ping on the event bus"}}, [this](const pi::CommandCall&) {
                                       m_host->emitEvent("hello:ping", pi::Json{{"from", "hello_commands"}});
                                       return std::string();
                                   }) +
                                   host.onEvent("hello:ping", [this](const pi::Json& data) { m_host->session("appendEntry", pi::Json{{"customType", "hello-ping"}, {"data", data}}); });
        if (!errors.empty()) {
            host.log(PI_LOG_ERROR, errors);
            return false;
        }
        return true;
    }

private:
    std::string note(const pi::CommandCall& call) const {
        if (call.args.empty()) {
            return "note needs text";
        }
        const pi::Json prefix = m_host->flag("note-prefix");
        std::string text = (prefix.is_string() ? prefix.get<std::string>() : std::string()) + call.args;
        if (m_host->flag("loud") == true) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        }
        const pi::Json result = m_host->session("appendEntry", pi::Json{{"customType", "hello-note"}, {"data", {{"text", text}}}});
        return result.value("error", "");
    }

    pi::Host* m_host = nullptr;
};

PI_PLUGIN(HelloCommandsPlugin)
