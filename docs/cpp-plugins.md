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
cancellation), `abort_requested`, `get_context` (`cwd`, `agentDir`), `register_provider`, `unregister_provider`, `register_mcp_server`, `unregister_mcp_server`, `register_virtual_model`, `unregister_virtual_model`, `list_models`, `register_stream_provider` and `stream_emit`.

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

### Stream-handler providers

`register_stream_provider(name, config, stream, user_data)` is the `streamSimple` provider of TypeScript extensions: the plugin
produces the responses itself instead of the host calling an HTTP API. `config` is a `register_provider` entry that must name
its wire API, `"api": "<name>"` (its models speak it), and carry an `apiKey` (any value, such as `"none"`, for a provider
without credentials; the key reaches the plugin in the request options). The API name must not be implemented already, so
built-in APIs cannot be replaced and two plugins cannot share a name. The registration ends with `unregister_provider` or when
the plugin unloads.

The host runs `stream(user_data, request_json, abort, sink)` once per request on a thread of its own. `request_json` is
`{"model":<catalog entry>,"messages":[...],"options":{"apiKey"?,"headers"?,"temperature"?,"maxTokens"?,"reasoning"?,"sessionId"?,"toolChoice"?,"cacheRetention"?,"metadata"?,"samplingParams"?,"timeoutMs"?}}`;
`messages` is the normalized transcript in the JSON of session entries, where the leading system messages carry the system
prompt and the tool definitions. The function reports the response with `stream_emit(sink, event_json)` and ends it with a
`done` or `error` event:

- `{"type":"text_delta","delta":".."}` and `{"type":"thinking_delta","delta":".."}`: consecutive deltas of one kind form one block;
- `{"type":"tool_call","id":"..","name":"..","arguments":{..}}`: a complete tool call (stop reason `toolUse` unless `done` says otherwise);
- `{"type":"usage","input":n,"output":n,"cacheRead"?:n,"cacheWrite"?:n}`: token counts; the cost comes from the model's prices;
- `{"type":"response","id"?:"..","model"?:".."}`: the provider's response id and the concrete model;
- `{"type":"done","stopReason"?:"stop"|"length"|"toolUse"}` and `{"type":"error","message":"..","aborted"?:bool}`.

The host builds the assistant message and the stream events with their `partial` snapshots from these (`PluginStreamTranslator`),
so plugins write deltas, not snapshots. `stream_emit` returns non-zero once the response is over: the request was cancelled
(`abort_requested(abort)` says the same), an earlier event ended it, or the event was invalid (the stream then ends with an
error naming the problem). A function that returns without a final event ends the response with an error, or `aborted` when
it was cancelled, so readers never wait forever. Unloading a plugin first cancels its running streams and waits for the
functions to return; do not call `unregister_provider` for a provider from inside its own stream function. The SDK wraps this
as `Host::registerStreamProvider(name, config, handler)` with a `pi::StreamSink` (`text`, `thinking`, `toolCall`, `usage`,
`response`, `done`, `error`); `plugins/hello_stream` is an example. `struct_size` must cover `stream_emit`.

Not carried over from the TypeScript handlers: `onPayload`/`onResponse` of the request options (no payload to inspect), and
the OAuth login of a provider (`oauth` in the config).

### MCP servers

`register_mcp_server(name, config)` registers an MCP server for the session with the config of an `mcpServers` entry of
`mcp.json` (stdio: `command`, `args`, `env`, `cwd`; HTTP: `url`, `headers`, `oauth`; `enabled`, `exposure`, `toolExposure`,
`timeout`). The server connects next to the configured ones (during plugin load it starts with them, later registrations
connect at once) and is withdrawn, tools and connection, when the plugin unloads or calls `unregister_mcp_server`. A server of the
same name in `mcp.json` wins and the registration is ignored; a name another plugin registered is refused; registering a
name again replaces the plugin's own. Nothing is saved: register again on every load. The registration is also ignored
when MCP is off (`--no-mcp`) or the tool set is restricted. Durable sessions (`pi serve`) support it; the session-tree
backend does not. `plugins/hello_mcp` is an example.

### Virtual models

`register_virtual_model(definition, route, user_data)` registers a selectable model whose requests a router maps to a physical model
(see `packages/coding-agent/docs/virtual-models.md`; `plugins/hello_router` is an example). The definition is
`{"provider", "id", "name"?, "thinkingLevels"?: ["low", "high"], "contextWindow"?, "maxTokens"?, "input"?}`: the model is listed
under `provider` (a provider with physical models, which must have credentials, or one nobody else defines, which needs none) and
appears in model selection like any other. `id` must not be a physical model of the provider. `unregister_virtual_model(provider, id)`
removes one; models also end with the plugin. `list_models()` returns the physical models whose provider has credentials
(`[{provider, id, name, reasoning, input, contextWindow, maxTokens}]`) for the router to choose from.

`route(request)` runs before every request made with the model. The request is `{model, thinkingLevel, reason, previous?, failed?,
state?, messages}`: `reason` is `user` (first request after a user message), `continuation` (after tool results), `retry`
(automatic retry; `failed` carries the physical model, level and failed assistant message) or `direct` (a compaction or branch
summary); `previous` is the physical model and level of the latest successful response; `state` is what the router returned
last. It answers `{"model": {"provider", "id"}, "thinkingLevel", "state"?}` or `{"error"}`; the model must be a physical model with
credentials, the thinking level is clamped to it, and a `state` that differs from the request's is stored on the session branch
(a `pi.virtual-model-state` custom entry in `pi rpc` sessions, a conversation entry of the same kind in `pi serve`), so forks and
resumed sessions see it. A failing router ends the request with an error response naming the virtual model. Assistant messages
name the physical model; the selection stays virtual, and a resumed session keeps the virtual selection while the model is
still registered (otherwise it falls back to the physical model that answered last). Context usage and compaction use the limits
of the physical model that produced the latest response. Not ported: the footer display of the routed model and `ctx.modelRegistry`
for routers (use `list_models`).

### Commands, flags, session calls and the event bus

`register_command(name, options, handler, user_data)` registers `/name args` (`options`: `{"description"?}`; the SDK's
`Host::registerCommand`; `plugins/hello_commands` is an example). A prompt that starts with `/name` runs the handler with the text
after the first space instead of reaching the model, before the `input` event and even while the agent streams; the prompt is
answered `Handled`. A command wins over a prompt template of the same name, and names are unique across plugins. A handler that
returns `{"error"}` makes the prompt fail with that message (TypeScript reports an extension error and swallows the prompt).
`get_commands` lists them with source `extension`. They are available in `pi rpc` sessions; `pi serve`
sessions do not dispatch them.

`register_flag(name, options)` declares `--name` (`{"description"?, "type": "boolean"|"string", "default"?}`) and `get_flag(name)`
reads its value (`{"value": ...}`: the command line value, else the default, else null). `pi rpc` and `pi serve` accept flags
they do not know (`--name`, `--name value`, `--name=value`); once the plugins are loaded they are checked against the declared
flags, and one no plugin declared (a plugin missing or disabled) becomes a startup warning diagnostic. A boolean flag takes no
value (`true`/`false` accepted); a string flag requires one.

`session_call(method, params, abort)` calls the session (the `ExtensionAPI`/`ExtensionContext` methods of TypeScript:
`sendMessage`, `sendUserMessage`, `appendEntry`, `setSessionName`, `setLabel`, tools, model and thinking level, `compact`,
`navigateTree`, `reload`, `waitForIdle`, `abort`, `sessionManager.*` reads, `getMcpServers`, ...; the full list is in
`pi_plugin.h`). It answers `{"error": "session not ready"}` during `pi_plugin_init`. `newSession`, `switchSession`, `fork` and
`shutdown` answer an error: replacing the session disposes the plugin while its handler is still running. Session calls
are available in `pi rpc` sessions.

`event_on(channel, handler, user_data)` and `event_emit(channel, data)` are the `pi.events` bus plugins share: handlers run on
the emitting thread in subscription order (including the emitter's own) and end with the plugin. All of these were appended to
`PiHostApi`; a plugin checks that `struct_size` covers `event_emit` (the SDK does).

### Hooks

`subscribe(event, handler)` registers a handler; it returns the result JSON, or nothing for "no opinion". Handlers
run synchronously in subscription order and see the changes of earlier handlers.

| Event | Payload | Result |
| --- | --- | --- |
| `tool_call` | `{toolCallId, toolName, input}` | `{block?, reason?, terminate?, input?}`; `input` replaces the arguments (not validated again) |
| `tool_result` | `{toolCallId, toolName, input, content, details, structuredContent, isError}` | `{content?, details?, structuredContent?, isError?}`; replacing `content` without `structuredContent` drops it |
| `context` | `{messages}` (session messages as stored in session files) | `{messages?}` replaces the messages sent to the model |
| `input` | `{text, images?, source, streamingBehavior?}` (`source` is `rpc`; `streamingBehavior` only while a run is active) | `{action: "continue"}`, `{action: "transform", text, images?}` or `{action: "handled"}`; transforms chain, `handled` ends the chain and swallows the input (before skill and template expansion) |
| `before_agent_start` | `{prompt, images?, systemPrompt}` (the rendered prompt) | `{message?: {customType, content?, display?, details?}, systemPrompt?}`; the message is sent with the prompt, `systemPrompt` replaces the complete system prompt for this turn (later handlers see it) |
| `before_provider_request` | `{payload}` (the provider request body) | any JSON replaces the payload; null keeps it |
| `session_before_compact` | `{preparation: {firstKeptEntryId, messagesToSummarize, turnPrefixMessages, isSplitTurn, tokensBefore, previousSummary?, fileOps, settings}, branchEntries, customInstructions?, reason, willRetry}` | `{cancel?, compaction?: {summary, firstKeptEntryId, tokensBefore, details?, usage?}}`; `cancel` ends the chain, `compaction` replaces the model call (stored as `fromHook`) |
| `session_compact`, `session_compact_failed` | `{compactionEntry, fromExtension, reason, willRetry}`, `{reason, errorMessage?, aborted, willRetry, fromExtension}` | none |
| `session_before_tree` | `{preparation: {targetId, oldLeafId, commonAncestorId, entriesToSummarize, userWantsSummary, customInstructions?, replaceInstructions, label?}}` | `{cancel?, summary?: {summary, details?, usage?}, customInstructions?, replaceInstructions?, label?}`; a summary is used only when the user asked for one |
| `session_tree` | `{newLeafId, oldLeafId, summaryEntry?, fromExtension?}` | none |
| `session_start`, `session_shutdown` | `{type, ...}` | none |
| agent events: `agent_start`, `agent_end`, `turn_start`, `turn_end`, `message_start`, `message_update`, `message_end`, `tool_execution_start`, `tool_execution_update`, `tool_execution_end`, and the session events of the RPC protocol | the event JSON of `pi rpc` | none (observation only) |

In `pi serve` (durable sessions) the same hooks are served from the durable hook points: `tool_call` before a tool task
runs (rewritten arguments are validated against the tool's schema afterwards), `tool_result` after it, `context` before
each model request, `message_end` after each model response and `turn_end` after each tool round. `before_provider_request`
runs as the provider builds its request. `input` and `before_agent_start` run in the `pi.agent-controller` service: handled
input answers `{accepted: false, error: {code: "input_handled"}}`, and `before_agent_start` runs for `prompt` only (not for
steering and follow-ups); a message plugins add travels as further text blocks of the prompt and a replacement
`systemPrompt` is ignored, because the durable prompt is assembled from registry sections. `session_before_compact` maps
onto the compaction task's `beforeCompact` hook (the payload carries the entries before the cut, the messages to
summarise and `firstKeptEntryId`; `cancel` declines the compaction and `compaction.summary` is placed at the cut the task
chose, so the plugin's `firstKeptEntryId` is ignored); `session_compact`, `session_compact_failed` and the tree events
are not delivered there. `terminate`, `structuredContent`, the session events and the other agent events are not delivered there either.

## Differences from TypeScript extensions

Not ported: UI APIs (`ctx.ui`, renderers, widgets), the OAuth login of providers (declarative and stream-handler providers work with API keys), commands in `pi serve` sessions, session replacement from plugins (`newSession`, `switchSession`, `fork`), the mutable `systemPromptOptions` of `before_agent_start` (plugins see and replace the rendered prompt), the other provider and session hooks (`before_provider_headers`, `after_provider_response`, `session_before_switch`, `session_before_fork`, ...). They can be added as new host API functions or events without breaking ABI version 1 because
the host API struct carries its size.

## Building a plugin

```
cc_binary(name = "my_plugin", srcs = ["my_plugin.cpp"], linkshared = True,
          deps = ["//sdk:pi_plugin_sdk", "//third_party:json"])
```

then `pi rpc --plugin bazel-bin/path/libmy_plugin.so`.
