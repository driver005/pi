# C++ port: build and run

The C++ tree (`interfaces/`, `src/`, `app/`, `tools/`) builds with Bazel 8 (bzlmod), clang-20 and libc++-20. The TypeScript packages are unchanged.

## Build and test

```
bazel test //...                      # unit tests
python3 tools/check_style.py .        # style gate (also a bazel test)
bazel build //app/pi:pi               # the executable
```

## Cross-checks against the TypeScript implementation

`tools/ts_cbor_golden.mts` runs the TS CBOR codec and frame encoder (Node 22, strip-types, no `npm install` needed) and writes `src/testing/ts_golden/ts_cbor_golden.json`. The C++ encoder, decoder and frame encoder tests read that file and must produce identical bytes and identical accept/reject decisions. Regenerate with `node --experimental-strip-types tools/ts_cbor_golden.mts src/testing/ts_golden/ts_cbor_golden.json`.

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
- Sessions: one directory per session under `--session-dir` (default `<agent-dir>/server-sessions`) with `meta.json` (`{createdAt, cwd}`, as in the TS server) and `session.sqlite`: a durable harness (`DurableSessionOpener`, `DurableServe` in the composition root) whose root conversation the services expose. pi's coding tools (`ToolBridge` over the `ITool`s of the conversation's directory) and system prompt (`PiPromptExtension`) are installed in a registry per session, the user's settings give the retry, compaction and queue policy, and tasks a stop interrupted resume when the session opens. The tools of plugins and MCP servers (`DurableToolbox`: plugins from the agent directory, the trusted project and `--plugin`, servers from `mcp.json`) are offered next to the built-in ones, and a server that connects late makes the sessions install their tools again; plugin hooks are not used by durable sessions. `--session-tree` keeps sessions as JSONL session trees over the AgentSession runtime instead (the two layouts must not share a session directory).
- Stop with SIGINT, SIGTERM or SIGHUP: clients are disconnected, running agents are aborted, the socket is removed.

Services (TS shapes, `packages/coding-agent/src/experimental/services`):

| Scope | Service | Notes |
|---|---|---|
| server | `pi.session-directory` | state `{revision, sessions: [{serverId, sessionId, createdAt}]}` |
| server | `pi.session-management` | `create({id?})`, `remove(id)`, `attach(id)`, `detach()`; id prefixes resolve |
| session | `pi.agent-controller` | `prompt`, `steer`, `followUp`, `cancelQueued`, `abort`, `compact`, `waitForPrompt`; replies `{accepted, operationId \| entryId, error}` |
| session | `pi.models` | state `{catalog, configuration, refresh}`, `select`, `selectThinking`, `cycleThinking`, `getThinkingLevels`, `refresh` |
| session | `pi.transcript` | durable sessions: state `{conversation, entries, docs}`, the durable `ConversationView` of the TS service; with `--session-tree`: `{messages, isStreaming}` (AgentSession messages in session-file JSON) |

With durable sessions `operationId` and `entryId` are submission ids (a compaction's `operationId` is its task id), so they survive a restart. `PresentationPlugins`, `SlashCommands`, `PresentationUI` and `SessionPlugins` are not provided. Sessions run in-process (one agent per session, shared by every attached client) rather than in a worker process each.

Requests run on a 64-thread pool and agent runs on a separate 32-thread pool, so calls that wait (`waitForPrompt`, `abort`) cannot starve the runs they wait for.

Offline smoke test:

```
PI_FAUX_REPLIES='["pong"]' pi serve --faux --server-dir /tmp/pi-server --server-id 00000000-0000-4000-8000-000000000001
```

## Durable storage (partial port of `packages/durable`)

`IStorage` (`interfaces/durable/i_storage`) is the atomic persistence boundary of the durable session: conversations, entries, tasks, submissions and versioned documents, with fork-aware entry scans, opaque cursors and as-of document reads. Records and writes are JSON in the shape of `packages/durable/src/types.ts`. Three backends exist:

- `MemoryStorage` (`src/durable/memory_storage`): the reference implementation; also an `IStagedStorage` (validate/apply in two steps).
- `JsonlStorage` (`src/durable/jsonl_storage`): `main.jsonl` commit markers plus `doc-<id>.jsonl` / `task-<id>.jsonl` sidecars, rebuilt into a memory store on open, with torn-tail and unconfirmed-record recovery and sidecar reclamation. Same on-disk format as the TS implementation (format 1).

- `SqliteStorage` (`src/durable/sqlite_storage`) over `ISqlDatabase` (`interfaces/durable/i_sql_database`; `SqliteDatabase` in `src/durable/sqlite_database` binds the system libsqlite3, WAL journal, immediate transactions). The schema and row encoding are those of the TS SQLite storage (`SqliteMigrations`), so the two implementations are meant to open each other's `session.sqlite` (not yet cross-checked against a file written by the TS server). Reads are SQL queries, not an in-memory copy.

All three pass the ported conformance suite (`src/testing/storage_conformance`); the harness restart-recovery test also runs over a SQLite file. The registry's `sqlite3` module downloads from sqlite.org, which the dev sandbox cannot reach, so `//third_party:sqlite3` links the system library (`libsqlite3-dev`); switch it to the registry module where the download works.

## Durable harness (port of `packages/durable` Session/Harness)

All of it lives in `interfaces/support` (logic classes that may hold threads and mutexes) over the `interfaces/durable` contracts; messages and entries stay `Json`, typed conversion happens only for provider calls (`MessageCodec` in `ModelRequests`).

- `DurableSession`: the single mutation line. `commit(callback, scope)` runs a `Transaction` (document drafts diffed into deltas, forks, task and submission rules, `read_after_write` refusal), appends one storage commit and returns its sequence. Readers use `snapshot`/`snapshotAsOf`/`readOnLine`; `subscribeCommits` delivers publications in order after the line, `subscribeCommitsOnLine` synchronously on it. A non-`storage_rejected` failure poisons the session.
- `TaskScheduler` with `Registry` (task kinds, builtin documents, migrations): one worker thread for reconcile/drain, one thread per invocation, ownership overlays, waiters, orphaning, `abort*`, `waitFor*`, `inspect`. Tasks left `running` by a crash return to `pending` when the scheduler opens.
- Built-in tasks `pi.generation`, `pi.tool`, `pi.compaction` (`GenerationTaskDefinition`, `ToolTaskDefinition`, `CompactionTaskDefinition`, registered by `BuiltinTasks`), with retry policy, output bounding, usage ledger, hooks/wraps and extension registration.
- `Harness` (`interfaces/support/harness`): `open`, `root`, `conversation(id)`, `createConversation` (forks), `getTask`, `submission`, `abortSubmission/abortTask`, `waitForTask/waitForIdle`, `usage`, `inspect`, `resume`. Conversations expose prompt/steer/follow-up submissions, the inbox boundary, compaction and `BoundConversation` handles.
- Observation: `Harness::viewState(conversationId)` returns an `IReplicatedState` of `{conversation, entries, docs}` (the active transcript entries and the `pi.agent`/`pi.live`/`pi.inbox`/`pi.usage` documents) and `Harness::taskGraph()` one of `{tasks: {id: node}}` for every live task (`ConversationViews`, `TaskGraphView`). A mount is built on the session line by its first holder, advances from the ordered commit publications (so it only shows committed state) and is dropped when the last holder releases it; its published ops are the diff of each commit, so replaying them over the first snapshot rebuilds the value.
- Agent events (spec section 9.4): `Harness::watchEvents(conversationId)` returns an `AgentEventWatch`: the `snapshot` event, then one batch of events per publication (`run_start`, `turn_start`, `message_start`/`message_update`/`message_end`, `tool_execution_*`, `submission`, `inbox_update`, `usage_changed`, ... in the order of the spec) delivered in order on a thread of its own, with the 100-batch overflow replaced by one snapshot. `AgentEventTranslator` derives the events from the mount's before/after values and the publication; message and tool output changes are computed from the values, so they do not depend on how the mount's diff aligned arrays.
- Tests run over `MemoryStorage` and over `JsonlStorage` on a fake file system (restart recovery), with `TaskFixture` (`src/testing/task_fixture`), the faux provider and a fixed clock; the scheduler, tasks, harness and submissions pass under `--config=tsan` and 20x stress runs.

A deferred provider response (stream option `deferred`) is polled through the `poll` phase of the generation task (`IProvider::fetchDeferred/cancelDeferred`, routed by `IModelRuntime`; only the faux provider implements it, as in TS) and an abort during polling cancels the handle.

Differences from TS: the exact-frame committed-state watches (`CommittedWatch` frames with operations) are not ported, and the view operations are diffs of the mount rather than the operations the documents recorded, so a consumer that replays them gets the same values but not necessarily the same operation sequence.

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

OAuth login flows; MCP resources and OAuth; plugin commands, providers and UI APIs; plugin hooks in durable sessions; a C++ protocol client; the `PresentationPlugins`/`SessionPlugins` services; HTML export; the package manager.
