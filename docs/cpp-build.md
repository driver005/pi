# C++ port: build and run

The C++ tree (`interfaces/`, `src/`, `app/`, `tools/`) builds with Bazel 8 (bzlmod), clang-20 and libc++-20. The TypeScript packages are unchanged.

## Build and test

```
bazel test //...                      # unit tests
python3 tools/check_style.py .        # style gate (also a bazel test)
bazel build //app/pi:pi               # the executable
```

## `pi rpc`

Headless JSONL server: one JSON command per line on stdin, one JSON line per response or event on stdout (LF framing). Command and event shapes follow `packages/coding-agent/src/modes/rpc/rpc-types.ts`.

```
pi rpc [--cwd DIR] [--agent-dir DIR] [--model provider/id[:level]] [--thinking LEVEL]
       [--continue | --session PATH|ID | --no-session] [--session-dir DIR]
       [--tools a,b | --no-tools] [--system-prompt TEXT|FILE] [--append-system-prompt TEXT|FILE]
       [--no-context-files] [--no-skills] [--no-prompt-templates]
       [--skill PATH] [--prompt-template PATH] [--trust | --no-trust] [--faux]
       [--plugin PATH] [--no-plugins] [--no-mcp] [--mcp-wait MS]
```

The agent directory is `$PI_CODING_AGENT_DIR` or `~/.pi/agent`. It holds `auth.json`, `models.json`, `settings.json`, `trust.json` and `sessions/`, in the same formats as the TypeScript implementation.

Offline smoke test with the scripted provider:

```
printf '%s\n' '{"id":"1","type":"prompt","message":"hi"}' |
  PI_FAUX_REPLIES='["hello"]' pi rpc --faux --no-session
```

## `pi serve`

Headless protocol server. Clients connect to a unix socket and speak protocol v8 (4-byte length-prefixed strict CBOR, `hello` handshake, request/cancel/response, `service_update` with Delta operations, `attachment`), the same wire protocol as `packages/protocol`.

```
pi serve [--server-dir DIR] [--server-id UUID] [--session-dir DIR] [--cwd DIR] [--agent-dir DIR]
         [--model provider/id[:level]] [--faux] [--no-mcp] [--no-plugins] ...
```

- Socket: `<server-dir>/<server-id>.sock` (mode 0600, directory 0700, same-user peers only). The server directory is `--server-dir`, `$PI_SERVER_DIR` or `~/.pi/server`. A stale socket file is replaced; a live one or a non-socket file is refused.
- Identity: `--server-id`, `$PI_SERVER_ID`, or the UUIDv4 kept in `<server-dir>/default-server-id`.
- Sessions: one directory per session under `--session-dir` (default `<agent-dir>/server-sessions`) with `meta.json` (`{createdAt, cwd}`, as in the TS server) and the session's JSONL tree. The TS server keeps `session.sqlite` there instead; the durable harness is not ported, so the two servers should not share a session directory.
- Stop with SIGINT, SIGTERM or SIGHUP: clients are disconnected, running agents are aborted, the socket is removed.

Services (TS shapes, `packages/coding-agent/src/experimental/services`):

| Scope | Service | Notes |
|---|---|---|
| server | `pi.session-directory` | state `{revision, sessions: [{serverId, sessionId, createdAt}]}` |
| server | `pi.session-management` | `create({id?})`, `remove(id)`, `attach(id)`, `detach()`; id prefixes resolve |
| session | `pi.agent-controller` | `prompt`, `steer`, `followUp`, `cancelQueued`, `abort`, `compact`, `waitForPrompt`; replies `{accepted, operationId \| entryId, error}` |
| session | `pi.models` | state `{catalog, configuration, refresh}`, `select`, `selectThinking`, `cycleThinking`, `getThinkingLevels`, `refresh` |
| session | `pi.transcript` | state `{messages, isStreaming}` (AgentSession messages in session-file JSON) |

`pi.transcript` differs from the TS service, which publishes the durable `ConversationView`; a TS presentation that renders that view does not work against it. `PresentationPlugins`, `SlashCommands`, `PresentationUI` and `SessionPlugins` are not provided. Sessions run in-process (one agent per session, shared by every attached client) rather than in a worker process each.

Requests run on a 64-thread pool and agent runs on a separate 32-thread pool, so calls that wait (`waitForPrompt`, `abort`) cannot starve the runs they wait for.

Offline smoke test:

```
PI_FAUX_REPLIES='["pong"]' pi serve --faux --server-dir /tmp/pi-server --server-id 00000000-0000-4000-8000-000000000001
```

## Durable storage (partial port of `packages/durable`)

`IStorage` (`interfaces/durable/i_storage`) is the atomic persistence boundary of the durable session: conversations, entries, tasks, submissions and versioned documents, with fork-aware entry scans, opaque cursors and as-of document reads. Records and writes are JSON in the shape of `packages/durable/src/types.ts`. Two backends exist:

- `MemoryStorage` (`src/durable/memory_storage`): the reference implementation; also an `IStagedStorage` (validate/apply in two steps).
- `JsonlStorage` (`src/durable/jsonl_storage`): `main.jsonl` commit markers plus `doc-<id>.jsonl` / `task-<id>.jsonl` sidecars, rebuilt into a memory store on open, with torn-tail and unconfirmed-record recovery and sidecar reclamation. Same on-disk format as the TS implementation (format 1).

Both pass the ported conformance suite (`src/testing/storage_conformance`). Not ported: the SQLite backend (sqlite3 cannot be fetched yet).

## Durable harness (port of `packages/durable` Session/Harness)

All of it lives in `interfaces/support` (logic classes that may hold threads and mutexes) over the `interfaces/durable` contracts; messages and entries stay `Json`, typed conversion happens only for provider calls (`MessageCodec` in `ModelRequests`).

- `DurableSession`: the single mutation line. `commit(callback, scope)` runs a `Transaction` (document drafts diffed into deltas, forks, task and submission rules, `read_after_write` refusal), appends one storage commit and returns its sequence. Readers use `snapshot`/`snapshotAsOf`/`readOnLine`; `subscribeCommits` delivers publications in order after the line, `subscribeCommitsOnLine` synchronously on it. A non-`storage_rejected` failure poisons the session.
- `TaskScheduler` with `Registry` (task kinds, builtin documents, migrations): one worker thread for reconcile/drain, one thread per invocation, ownership overlays, waiters, orphaning, `abort*`, `waitFor*`, `inspect`. Tasks left `running` by a crash return to `pending` when the scheduler opens.
- Built-in tasks `pi.generation`, `pi.tool`, `pi.compaction` (`GenerationTaskDefinition`, `ToolTaskDefinition`, `CompactionTaskDefinition`, registered by `BuiltinTasks`), with retry policy, output bounding, usage ledger, hooks/wraps and extension registration.
- `Harness` (`interfaces/support/harness`): `open`, `root`, `conversation(id)`, `createConversation` (forks), `getTask`, `submission`, `abortSubmission/abortTask`, `waitForTask/waitForIdle`, `usage`, `inspect`, `resume`. Conversations expose prompt/steer/follow-up submissions, the inbox boundary, compaction and `BoundConversation` handles.
- Observation: `Harness::viewState(conversationId)` returns an `IReplicatedState` of `{conversation, entries, docs}` (the active transcript entries and the `pi.agent`/`pi.live`/`pi.inbox`/`pi.usage` documents) and `Harness::taskGraph()` one of `{tasks: {id: node}}` for every live task (`ConversationViews`, `TaskGraphView`). A mount is built on the session line by its first holder, advances from the ordered commit publications (so it only shows committed state) and is dropped when the last holder releases it; its published ops are the diff of each commit, so replaying them over the first snapshot rebuilds the value.
- Tests run over `MemoryStorage` and over `JsonlStorage` on a fake file system (restart recovery), with `TaskFixture` (`src/testing/task_fixture`), the faux provider and a fixed clock; the scheduler, tasks, harness and submissions pass under `--config=tsan` and 20x stress runs.

A deferred provider response (stream option `deferred`) is polled through the `poll` phase of the generation task (`IProvider::fetchDeferred/cancelDeferred`, routed by `IModelRuntime`; only the faux provider implements it, as in TS) and an abort during polling cancels the handle.

Differences from TS: the agent event stream and the exact-frame committed-state watches are not ported, and `pi serve` still uses the JSONL session store and its own `Transcript` shape rather than publishing the durable `ConversationView` through the `Transcript` service.

## MCP servers

Servers are read from `<agent-dir>/mcp.json` and, for trusted projects, `<cwd>/.pi/mcp.json`, in the shared `mcpServers` shape (`command`/`args`/`env`/`cwd` for stdio, `url`/`headers` for streamable HTTP; `enabled`, `exposure`, `toolExposure`, `timeout`, `description`). Project entries replace global ones; a project entry with only `enabled`, `exposure` or `toolExposure` overrides the global server and keeps its credentials. `${VAR}` and `!cmd` values in `env` and `headers` are resolved at connect time; `auth.provider` sends a pi provider's token.

Servers connect in parallel at session start. Startup waits at most `--mcp-wait` milliseconds (default 5000); slower servers add their tools when ready. Tools are named `mcp__<server>__<tool>` (sanitized, at most 64 characters, hash suffix on collisions) and are active alongside the built-in tools, unless `--tools`/`--no-tools` or a `defaultTools` setting restricts the tool set. Results over 20KB keep start and end; the full text goes to a private temp file. A dropped connection reconnects on the next call.

Differences from the TypeScript implementation: codemode and `tool_search` are not ported, so every exposure except `hidden` offers tools directly; MCP resources, OAuth sign-in and server log files are not ported (use an `Authorization` header or `auth.provider`).

## Plugins

Shared libraries loaded through a C ABI replace TypeScript extensions: they add tools and subscribe to hooks (`tool_call`, `tool_result`, `context`, agent events). See [cpp-plugins.md](cpp-plugins.md).

## Wiring

`app/` is the composition root and the only place that constructs concrete classes:

- `PlatformServices`: clock, ids, files, locks, processes, crypto, HTTP, executor.
- `ModelServices`: credential and models.json stores, providers, `ModelRuntime`.
- `CodingServices`: the above plus the shared agent loop, session store and trust resolver.
- `CodingSessionHandle`: per-session settings (project settings only when trusted), resources, tools and the `AgentSession`.
- `PluginHost` (in `src/plugin`) and the `HookBus` of each session: plugin tools and hooks.
- `McpServerManager` and `McpConnector`: connect the `mcp.json` servers, register their tools and build their transports.
- `CodingRuntimeFactory` and `CodingApplication`: session replacement (new, switch, fork) and the RPC run mode.

## Not yet ported

OAuth login flows; MCP resources and OAuth; plugin commands, providers and UI APIs; SQLite storage; the durable agent event stream and the TS-compatible `Transcript` service on the durable harness; a C++ protocol client; the `PresentationPlugins`/`SessionPlugins` services; HTML export; the package manager.
