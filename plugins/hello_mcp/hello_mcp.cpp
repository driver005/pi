// An example plugin: registers the MCP server "hello-mcp", a stdio server (a shell script) with one `echo` tool.
#include "pi_plugin.hpp"

class HelloMcpPlugin : public pi::Plugin {
public:
    bool init(pi::Host& host) override {
        const std::string script = R"sh(
while IFS= read -r line; do
  id=${line#*\"id\":}; id=${id%%,*}
  case "$line" in
    *'"method":"initialize"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"hello-mcp","version":"1"}}}\n' "$id";;
    *'"method":"tools/list"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[{"name":"echo","description":"Echo","inputSchema":{"type":"object"}}]}}\n' "$id";;
  esac
done
)sh";
        const std::string error = host.registerMcpServer("hello-mcp", pi::Json{{"command", "/bin/sh"}, {"args", pi::Json::array({"-c", script})}});
        if (!error.empty()) {
            host.log(PI_LOG_ERROR, error);
            return false;
        }
        return true;
    }
};

PI_PLUGIN(HelloMcpPlugin)
