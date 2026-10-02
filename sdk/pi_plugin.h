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
} PiHostApi;

typedef uint32_t (*PiPluginAbiVersionFn)(void);
typedef int (*PiPluginInitFn)(const PiHostApi* host);
typedef void (*PiPluginShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif
