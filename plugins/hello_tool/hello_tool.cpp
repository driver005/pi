// An example plugin: one `hello` tool, a hook that blocks `rm -rf` bash commands, and a log line.
#include "pi_plugin.hpp"

class HelloPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        host.log(PI_LOG_INFO, "hello plugin loaded");
        const std::string toolError = host.registerTool(
            pi::Json{{"name", "hello"},
                     {"description", "Greets someone by name."},
                     {"promptSnippet", "Greet someone by name"},
                     {"parameters", pi::Json{{"type", "object"},
                                             {"properties", pi::Json{{"name", pi::Json{{"type", "string"}}}}},
                                             {"required", pi::Json::array({"name"})}}}},
            [](const pi::ToolCall& call) {
                call.update("looking for " + call.params.value("name", std::string("nobody")));
                return pi::text("Hello, " + call.params.value("name", std::string("stranger")) + "!");
            });
        if (!toolError.empty()) {
            host.log(PI_LOG_ERROR, toolError);
            return false;
        }
        host.on("tool_call", [](const pi::Json& payload) -> pi::Json {
            const pi::Json input = payload.value("input", pi::Json::object());
            const std::string command = input.value("command", std::string());
            if (payload.value("toolName", std::string()) == "bash" && command.find("rm -rf") != std::string::npos) {
                return pi::Json{{"block", true}, {"reason", "hello plugin refuses rm -rf"}};
            }
            return nullptr;
        });
        return true;
    }
};

PI_PLUGIN(HelloPlugin)
