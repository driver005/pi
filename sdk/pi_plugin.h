/* style:c-abi */
/*
 * pi plugin ABI, version 1.
 *
 * A plugin is a shared library (.so / .dylib) exporting three C symbols:
 *
 *     uint32_t pi_plugin_abi_version(void);          returns PI_PLUGIN_ABI_VERSION
 *     int      pi_plugin_init(const PiHostApi* host); 0 on success; the host pointer stays valid until shutdown
 *     void     pi_plugin_shutdown(void);
 *
 * Everything crossing the boundary is plain C. Structured data travels as UTF-8 JSON in PiString
 * (borrowed, valid for the duration of the call) or PiOwnedString (the producer allocates; the
 * consumer calls `release` when it is non-NULL). Callbacks run on the host's threads, possibly
 * concurrently; a plugin must make its callbacks thread-safe and must not throw across the boundary.
 *
 * Tool execute callback result (JSON): {"content":[{"type":"text","text":"..."} |
 *   {"type":"image","data":"<base64>","mimeType":"image/png"}], "details":any, "structuredContent":any,
 *   "isError":bool, "terminate":bool}   or   {"error":"message"} to fail the call.
 *
 * Hook events and their JSON payloads/results are documented in docs/cpp-plugins.md.
 */
#ifndef PI_PLUGIN_H
#define PI_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI_PLUGIN_ABI_VERSION 1u

#define PI_LOG_DEBUG 0
#define PI_LOG_INFO 1
#define PI_LOG_WARNING 2
#define PI_LOG_ERROR 3

typedef struct PiString {
    const char* data;
    size_t size;
} PiString;

typedef struct PiOwnedString {
    char* data;
    size_t size;
    void (*release)(char* data, size_t size);
} PiOwnedString;

/* Cancellation of one tool call or exec; query it with PiHostApi.abort_requested. */
typedef struct PiAbort PiAbort;

/* Streams a partial tool result: {"content":[...], "details":any}. */
typedef void (*PiUpdateFn)(void* context, PiString partial_result_json);

typedef PiOwnedString (*PiToolExecuteFn)(void* user_data, PiString tool_call_id, PiString params_json,
                                         const PiAbort* abort, PiUpdateFn on_update, void* update_context);

/* Returns the hook's result JSON, or an empty PiOwnedString (data NULL) for "no opinion". */
typedef PiOwnedString (*PiHookFn)(void* user_data, PiString event, PiString payload_json);

/*
 * Routes one request of a virtual model: returns {"model":{"provider","id"},"thinkingLevel":"low","state"?:any} or
 * {"error":"..."}; an empty PiOwnedString (data NULL) fails the request. request_json is {"model":<catalog entry of the
 * virtual model>, "thinkingLevel", "reason":"user"|"continuation"|"retry"|"direct", "previous"?:{"model","thinkingLevel"?},
 * "failed"?:{"model","thinkingLevel"?,"message"}, "state"?:any, "messages":[...]}. `abort` is cancelled with the request.
 * The answer must name a physical model of a provider with credentials (see list_models); state is stored on the session
 * branch when it differs from the request's.
 */
typedef PiOwnedString (*PiRouteFn)(void* user_data, PiString request_json, const PiAbort* abort);

/*
 * Runs a command a plugin registered when the user prompts `/<name> <args>` (args: the text after the first space, possibly
 * empty); the prompt is not sent to the model. Returns an empty PiOwnedString (data NULL) on success or {"error":".."}.
 */
typedef PiOwnedString (*PiCommandFn)(void* user_data, PiString args, const PiAbort* abort);

/* One response being streamed by a provider's PiStreamFn; valid until that function returns. */
typedef struct PiStreamSink PiStreamSink;

/*
 * Streams one response of a stream-handler provider (see register_stream_provider). Runs on a thread of its own until it
 * returns; it reports the response through PiHostApi.stream_emit(sink, event_json) and must end it with a "done" or "error"
 * event. request_json is {"model":<catalog entry>, "messages":[...the normalized transcript: leading system messages carry the
 * prompt and the tools...], "options":{"apiKey"?, "headers"?:{..}, "temperature"?, "maxTokens"?, "reasoning"?:"off|minimal|low|
 * medium|high|xhigh", "sessionId"?, "toolChoice"?, "cacheRetention"?, "metadata"?, "samplingParams"?, "timeoutMs"?}}. `abort`
 * is cancelled with the request; stop producing then (stream_emit also reports it).
 */
typedef void (*PiStreamFn)(void* user_data, PiString request_json, const PiAbort* abort, PiStreamSink* sink);

typedef struct PiHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    void* host;

    void (*log)(void* host, int level, PiString message);

    /*
     * definition: {"name":..., "description":..., "parameters":<JSON Schema>, "label"?:..,
     * "promptSnippet"?:.., "promptGuidelines"?:[..], "executionMode"?:"sequential"|"parallel"}.
     * Returns {"ok":true} or {"error":"..."}.
     */
    PiOwnedString (*register_tool)(void* host, PiString definition_json, PiToolExecuteFn execute,
                                   void* user_data);

    /* Subscribes to a hook event by name. Returns {"ok":true} or {"error":"..."}. */
    PiOwnedString (*subscribe)(void* host, PiString event, PiHookFn handler, void* user_data);

    /* Non-zero once the call that owns `abort` was cancelled. NULL is never aborted. */
    int (*abort_requested)(const PiAbort* abort);

    /*
     * Runs a program: {"command":..,"args"?:[..],"cwd"?:..,"env"?:{..},"stdin"?:..,"timeoutMs"?:..}.
     * Returns {"exitCode":n,"output":<stdout and stderr merged>,"timedOut":bool,"aborted":bool}
     * or {"error":"..."} when the program could not be started.
     */
    PiOwnedString (*exec)(void* host, PiString request_json, const PiAbort* abort);

    /* {"cwd":..., "agentDir":...} */
    PiOwnedString (*get_context)(void* host);

    /*
     * Added after the first release of ABI 1: a plugin that uses the functions below checks that
     * `struct_size` covers them (offsetof(PiHostApi, unregister_provider) + sizeof(void*)).
     *
     * Registers (or replaces) a model provider. config_json is a models.json provider entry, the declarative
     * ProviderConfig of TypeScript extensions without stream handlers: {"baseUrl"?, "apiKey"?, "api"?, "headers"?,
     * "models"?:[{"id", "name"?, "api"?, "reasoning"?, "input"?, "cost"?, "contextWindow"?, "maxTokens"?, ...}], ...}.
     * Without "models" it overrides the settings of an existing provider. The registration ends when the plugin is
     * unloaded. Returns {"ok":true} or {"error":"..."}.
     */
    PiOwnedString (*register_provider)(void* host, PiString name, PiString config_json);

    /* Removes a provider this plugin registered. Returns {"ok":true} or {"error":"..."}. */
    PiOwnedString (*unregister_provider)(void* host, PiString name);

    /*
     * Registers an MCP server for the session, with the config of an `mcpServers` entry of mcp.json (stdio: command, args,
     * env, cwd; HTTP: url, headers, oauth; enabled, exposure, toolExposure, timeout). It connects next to the configured
     * servers and is withdrawn when the plugin unloads. A server of the same name in mcp.json takes precedence; a name
     * another plugin registered is refused. Registering a name again replaces the plugin's earlier registration.
     * Returns {"ok":true} or {"error":"..."}. Check `struct_size` covers `unregister_mcp_server` before calling.
     */
    PiOwnedString (*register_mcp_server)(void* host, PiString name, PiString config_json);

    /* Removes an MCP server this plugin registered and closes its connection. Returns {"ok":true} or {"error":"..."}. */
    PiOwnedString (*unregister_mcp_server)(void* host, PiString name);

    /*
     * Registers (or replaces) a virtual model: a selectable model whose requests `route` maps to a physical model.
     * definition_json: {"provider","id","name"?,"thinkingLevels"?:["low","high"],"contextWindow"?,"maxTokens"?,
     * "input"?:["text","image"]}. `provider` may be a provider with physical models or one that does not exist (then the
     * model needs no credentials); `id` must not be a physical model of the provider. Ends when the plugin is unloaded.
     * Returns {"ok":true} or {"error":"..."}. Check `struct_size` covers `list_models` before calling these three.
     */
    PiOwnedString (*register_virtual_model)(void* host, PiString definition_json, PiRouteFn route, void* user_data);

    /* Removes a virtual model this plugin registered. Returns {"ok":true} or {"error":"..."}. */
    PiOwnedString (*unregister_virtual_model)(void* host, PiString provider, PiString id);

    /* The physical models whose provider has credentials: [{"provider","id","name","reasoning","input","contextWindow","maxTokens"}]. */
    PiOwnedString (*list_models)(void* host);

    /*
     * Registers a provider that streams its responses itself (the `streamSimple` provider of TypeScript extensions).
     * config_json is as for register_provider and must name the wire API its models speak, `"api": "<name>"`, which must not
     * be implemented already (built-in APIs cannot be replaced); the usual provider fields apply: models, and an "apiKey"
     * (any value, e.g. "none", for a provider without credentials; the key reaches `stream` in options). `stream` runs once
     * per request. The registration ends with unregister_provider or when the plugin is unloaded, which first cancels the
     * running streams and waits for them to return. Returns {"ok":true} or {"error":"..."}. Check `struct_size` covers
     * `stream_emit` before calling these.
     */
    PiOwnedString (*register_stream_provider)(void* host, PiString name, PiString config_json, PiStreamFn stream, void* user_data);

    /*
     * Reports one event of the response being streamed; returns 0, or non-zero when the response is over (cancelled, or
     * ended by an earlier event or an invalid one) and the function should return. Events, each a JSON object:
     *   {"type":"text_delta","delta":"..."}                          appends to the current text block
     *   {"type":"thinking_delta","delta":"..."}                      appends to the current reasoning block
     *   {"type":"tool_call","id":"..","name":"..","arguments":{..}}  a complete tool call
     *   {"type":"usage","input":n,"output":n,"cacheRead"?:n,"cacheWrite"?:n}   the response's token counts (cost is computed)
     *   {"type":"response","id"?:"..","model"?:".."}                 the provider's response id and the concrete model
     *   {"type":"done","stopReason"?:"stop"|"length"|"toolUse"}      the response succeeded (default "stop")
     *   {"type":"error","message":"..","aborted"?:bool}              the response failed
     * Consecutive deltas of one kind form one block. Call it from the thread running `stream` only.
     */
    int (*stream_emit)(PiStreamSink* sink, PiString event_json);

    /*
     * Calls the session: `method` names an operation and params_json holds its arguments (an object; "{}" for none). Returns
     * the result JSON or {"error":"..."}; before the session exists (during pi_plugin_init) every call answers
     * {"error":"session not ready"}. Methods (the ExtensionAPI and ExtensionContext of TypeScript extensions):
     *   sendMessage {customType, content, display?, details?, deliverAs?: "steer"|"followUp"|"nextTurn", triggerTurn?}
     *   sendUserMessage {text, deliverAs?: "steer"|"followUp"}                   prompts the model (queued while streaming)
     *   appendEntry {customType, data?}      setSessionName {name}      getSessionName {}      setLabel {entryId, label?}
     *   getActiveTools {}      getAllTools {}      setActiveTools {names}      getCommands {}      getSettings {}
     *   getModel {}      setModel {provider, id} -> {ok}      getThinkingLevel {}      setThinkingLevel {level}
     *   isIdle {}  hasPendingMessages {}  isProjectTrusted {}  abort {}  getContextUsage {}  getSystemPrompt {}
     *   compact {customInstructions?}        starts a compaction and returns at once
     *   shutdown {}                          asks the host to end the session
     *   waitForIdle {}  (newSession, switchSession and fork answer an error: they would dispose the plugin mid-call)
     *   navigateTree {targetId, summarize?, customInstructions?, label?}  reload {}
     *   sessionManager.getEntries {}  .getBranch {fromId?}  .getEntry {id}  .getLeafId {}  .getSessionId {}  .getCwd {}
     *   getMcpServers {}                     the MCP servers and their state
     * `abort` (may be NULL) cancels waiting methods (waitForIdle, compaction waits). Check `struct_size` covers `event_emit`.
     */
    PiOwnedString (*session_call)(void* host, PiString method, PiString params_json, const PiAbort* abort);

    /*
     * Registers a command (see PiCommandFn). options_json: {"description"?:".."}. Names are unique across plugins (the later
     * registration is refused) and win over a prompt template of the same name. Ends with the plugin. Returns {"ok":true} or
     * {"error":".."}.
     */
    PiOwnedString (*register_command)(void* host, PiString name, PiString options_json, PiCommandFn handler, void* user_data);

    /*
     * Declares a command line flag `--<name>`: options_json {"description"?, "type": "boolean"|"string", "default"?}. The value
     * the user passed (or the default) is read with get_flag. A command line flag no plugin declares is reported as a startup warning.
     */
    PiOwnedString (*register_flag)(void* host, PiString name, PiString options_json);

    /* {"value": true|false|"text"|null} for a flag this plugin registered (null: not given and no default). */
    PiOwnedString (*get_flag)(void* host, PiString name);

    /*
     * The event bus plugins share (the `pi.events` of TypeScript extensions): handler(user_data, channel, data_json) runs for
     * every event_emit on the channel, in subscription order, on the emitting thread; its result is ignored. Subscriptions
     * end with the plugin.
     */
    PiOwnedString (*event_on)(void* host, PiString channel, PiHookFn handler, void* user_data);

    /* Emits data on a channel to every subscriber (including the emitting plugin). Returns {"ok":true}. */
    PiOwnedString (*event_emit)(void* host, PiString channel, PiString data_json);
} PiHostApi;

typedef uint32_t (*PiPluginAbiVersionFn)(void);
typedef int (*PiPluginInitFn)(const PiHostApi* host);
typedef void (*PiPluginShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif
