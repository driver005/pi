# C++ plugins

Plugins replace the TypeScript extension system in the C++ port. A plugin is a shared library
(`.so`, `.dylib`) that talks to the host through a C ABI (`sdk/pi_plugin.h`); `sdk/pi_plugin.hpp` is a
header-only C++ wrapper over it (needs nlohmann/json). `plugins/hello_tool` is a complete example.

## Loading

The host loads, in order:

1. every `*.so` / `*.dylib` in `<agent-dir>/plugins/`;
2. the same in `<cwd>/.pi/plugins/`, only when the project is trusted (like project settings and `mcp.json`);
3. every `--plugin <path>` (repeatable).

`--no-plugins` skips all of them. A plugin that fails to load (missing symbols, ABI mismatch, `pi_plugin_init`
returning non-zero) is reported as a diagnostic; the others still load. Plugin tools become active next to the
built-in tools unless `--tools`, `--no-tools` or a `defaultTools` setting restricts the tool set.

## ABI

A plugin exports three symbols:

```c
uint32_t pi_plugin_abi_version(void);           /* PI_PLUGIN_ABI_VERSION */
int      pi_plugin_init(const PiHostApi* host); /* 0 on success; host stays valid until shutdown */
void     pi_plugin_shutdown(void);              /* optional */
```

Data crosses the boundary as UTF-8 JSON in `PiString` (borrowed for the call) or `PiOwnedString` (the producer
allocates, the consumer calls `release` when set). Callbacks may run on any host thread, concurrently; they must
not throw across the boundary. The host API offers `log`, `register_tool`, `subscribe`, `exec` (run a program, with
cancellation), `abort_requested`, `get_context` (`cwd`, `agentDir`), `register_provider`, `unregister_provider`, `register_mcp_server` and `unregister_mcp_server`.

### Tools

`register_tool` takes `{"name","description","parameters"(JSON Schema),"label"?,"promptSnippet"?,"promptGuidelines"?,"executionMode"?}`
and an execute callback. The callback receives the call id, the validated arguments, an abort handle and an update
function for partial results, and returns `{"content":[{"type":"text","text":..}|{"type":"image","data":..,"mimeType":..}],"details"?,"structuredContent"?,"isError"?,"terminate"?}`
or `{"error":"message"}` to fail the call.

### Providers

`register_provider(name, config)` registers or replaces a model provider with a `models.json` provider entry:
`{"baseUrl"?,"apiKey"?,"api"?,"headers"?,"models"?:[{"id","name"?,"api"?,"reasoning"?,"input"?,"cost"?,"contextWindow"?,"maxTokens"?}]}`,
the declarative `ProviderConfig` of TypeScript extensions (`apiKey` may be a literal, `$ENV` or `!command`). Without `models`
it overrides settings of an existing provider. The registration ends when the plugin unloads (or with
`unregister_provider`), which restores overridden built-in providers. The two functions were added to the end of
`PiHostApi`; a plugin checks `struct_size` before calling them (the SDK's `Host::registerProvider` does). Hosts without a
model registry answer `{"error":...}`. `plugins/hello_provider` is an example.

### MCP servers

`register_mcp_server(name, config)` registers an MCP server for the session with the config of an `mcpServers` entry of
`mcp.json` (stdio: `command`, `args`, `env`, `cwd`; HTTP: `url`, `headers`, `oauth`; `enabled`, `exposure`, `toolExposure`,
`timeout`). The server connects next to the configured ones (during plugin load it starts with them, later registrations
connect at once) and is withdrawn, tools and connection, when the plugin unloads or calls `unregister_mcp_server`. A server of the
same name in `mcp.json` wins and the registration is ignored; a name another plugin registered is refused; registering a
name again replaces the plugin's own. Nothing is saved: register again on every load. The registration is also ignored
when MCP is off (`--no-mcp`) or the tool set is restricted. Durable sessions (`pi serve`) support it; the session-tree
backend does not. `plugins/hello_mcp` is an example.

### Hooks

`subscribe(event, handler)` registers a handler; it returns the result JSON, or nothing for "no opinion". Handlers
run synchronously in subscription order and see the changes of earlier handlers.

| Event | Payload | Result |
| --- | --- | --- |
| `tool_call` | `{toolCallId, toolName, input}` | `{block?, reason?, terminate?, input?}`; `input` replaces the arguments (not validated again) |
| `tool_result` | `{toolCallId, toolName, input, content, details, structuredContent, isError}` | `{content?, details?, structuredContent?, isError?}`; replacing `content` without `structuredContent` drops it |
| `context` | `{messages}` (session messages as stored in session files) | `{messages?}` replaces the messages sent to the model |
| `session_start`, `session_shutdown` | `{type, ...}` | none |
| agent events: `agent_start`, `agent_end`, `turn_start`, `turn_end`, `message_start`, `message_update`, `message_end`, `tool_execution_start`, `tool_execution_update`, `tool_execution_end`, and the session events of the RPC protocol | the event JSON of `pi rpc` | none (observation only) |

In `pi serve` (durable sessions) the same hooks are served from the durable hook points: `tool_call` before a tool task
runs (rewritten arguments are validated against the tool's schema afterwards), `tool_result` after it, `context` before
each model request, `message_end` after each model response and `turn_end` after each tool round. `terminate`,
`structuredContent`, the session events and the other agent events are not delivered there.

## Differences from TypeScript extensions

Not ported: UI APIs (`ctx.ui`, renderers, widgets), providers with their own stream handlers or OAuth and virtual models (declarative providers work), commands and flags, the `input`, `before_agent_start`, `before_provider_request` and compaction/tree hooks, and the shared event bus
between extensions. They can be added as new host API functions or events without breaking ABI version 1 because
the host API struct carries its size.

## Building a plugin

```
cc_binary(name = "my_plugin", srcs = ["my_plugin.cpp"], linkshared = True,
          deps = ["//sdk:pi_plugin_sdk", "//third_party:json"])
```

then `pi rpc --plugin bazel-bin/path/libmy_plugin.so`.
