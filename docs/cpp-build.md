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

`tools/ts_delta_golden.mts` does the same for the Delta code of `packages/chord`: `apply` (244 cases incl. rejections), op and wire validation, the stateful path-dictionary `encoder()`/`decoder()` over multi-batch streams, and `diffRevisions` (204 revision pairs, incl. the 4096-operation cap). The C++ applier, validator, encoder, decoder and differ reproduce every vector exactly. One deliberate difference: TS aligns array elements by object identity when a caller reuses containers between revisions; C++ values have no identity, so containers never anchor and the output equals TS for revisions that crossed a serialisation boundary. Regenerate with `node --experimental-strip-types tools/ts_delta_golden.mts src/testing/ts_golden/ts_delta_golden.json`.

`tools/ts_crosscheck/run.sh` is a manual end-to-end check (needs `npm install --ignore-scripts` and a built `bazel-bin/app/pi/pi`; not part of `bazel test`). It bundles the TS sources with esbuild (`source` export condition, protocol/client aliased to their `src`) and:

1. drives a real `pi serve --faux` (both `--session-tree` and the durable default) with the TS `packages/client` over the Unix transport: hello, service catalogue, `pi.session-directory` subscription (Delta updates decoded by the TS chord decoder), `create`/`attach`, `pi.transcript` subscription, `prompt` + `waitForPrompt`, subscription to every session service, and, for the durable backend, a server restart followed by re-attach with the transcript intact;
2. copies the C++-written `session.sqlite` through the TS `SqliteStorage` (`openNodeSqliteStorage`: the TS migrations accept the file, the TS reader returns the C++ entries, tasks and documents) into a new file written by TS, then starts the C++ server over that file as a session directory, reads its transcript through the TS client and continues the conversation with a new prompt.

No wire or storage discrepancy was found. A hand-made TS file lacking the `pi.agent` document yields `no_model` for prompts (the TS worker creates that document at session creation, so this is expected).

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
- Sessions: one directory per session under `--session-dir` (default `<agent-dir>/server-sessions`) with `meta.json` (`{createdAt, cwd}`, as in the TS server) and `session.sqlite`: a durable harness (`DurableSessionOpener`, `DurableServe` in the composition root) whose root conversation the services expose. pi's coding tools (`ToolBridge` over the `ITool`s of the conversation's directory) and system prompt (`PiPromptExtension`) are installed in a registry per session, the user's settings give the retry, compaction and queue policy, and tasks a stop interrupted resume when the session opens. The tools of plugins and MCP servers (`DurableToolbox`, reloadable through the session's `pi.session-plugins` service: plugins from the agent directory, the trusted project and `--plugin`, servers from `mcp.json`) are offered next to the built-in ones, and a server that connects late makes the sessions install their tools again; the hooks of those plugins reach the sessions of their directory through the `plugin-hooks` extension (`PluginHookExtension`): `tool_call` (block or rewrite arguments, from `beforeTool`), `tool_result` (replace content, details or the error flag, from `afterTool`), `context` (replace the request messages, from `beforeRequest`) and the observations `message_end` and `turn_end`; the `terminate` flag of `tool_call` and the other TS events are not mapped. `--session-tree` keeps sessions as JSONL session trees over the AgentSession runtime instead (the two layouts must not share a session directory).
- Stop with SIGINT, SIGTERM or SIGHUP: clients are disconnected, running agents are aborted, the socket is removed.

Services (TS shapes, `packages/coding-agent/src/experimental/services`):

| Scope | Service | Notes |
|---|---|---|
| server | `pi.session-directory` | state `{revision, sessions: [{serverId, sessionId, createdAt}]}` |
| server | `pi.session-management` | `create({id?})`, `remove(id)`, `attach(id)`, `detach()`; id prefixes resolve |
| session | `pi.agent-controller` | `prompt`, `steer`, `followUp`, `cancelQueued`, `abort`, `compact`, `waitForPrompt`; replies `{accepted, operationId \| entryId, error}` |
| session | `pi.models` | state `{catalog, configuration, refresh}`, `select`, `selectThinking`, `cycleThinking`, `getThinkingLevels`, `refresh` |
| session | `pi.transcript` | durable sessions: state `{conversation, entries, docs}`, the durable `ConversationView` of the TS service; with `--session-tree`: `{messages, isStreaming}` (AgentSession messages in session-file JSON) |

With durable sessions `operationId` and `entryId` are submission ids (a compaction's `operationId` is its task id), so they survive a restart. `PresentationPlugins` (empty selection) is provided server-wide and `SessionPlugins` by durable sessions (see Plugin services); `SlashCommands` and `PresentationUI` are client-side services of the TS TUI and not provided. Sessions run in-process (one agent per session, shared by every attached client) rather than in a worker process each.

Requests run on a 64-thread pool and agent runs on a separate 32-thread pool, so calls that wait (`waitForPrompt`, `abort`) cannot starve the runs they wait for.

Offline smoke test:

```
PI_FAUX_REPLIES='["pong"]' pi serve --faux --server-dir /tmp/pi-server --server-id 00000000-0000-4000-8000-000000000001
```

## Plugin services

`pi.presentation-plugins` (server-wide, one per client; `prepareSession`, `reload`) answers the empty selection `{presentationFacetBundles: []}` after the same checks as TS (session exists, `reload` needs a prepared selection, detaching clears it): presentation plugins are JavaScript bundles and this server only hosts native plugins. `pi.session-plugins` (per durable session; `reload`) unloads the plugins of the session's directory and loads them again (`DurableToolbox::reload`), which replaces their tools and hooks in every session of that directory. Nothing may be running a plugin tool during a reload. The session-tree backend does not offer `pi.session-plugins`.

## Protocol client

`ProtocolClient` (`interfaces/support/protocol_client`) is the C++ counterpart of `packages/client`: it connects over an `IClientTransport` (`PosixUnixConnector` in `src/base` for unix sockets; `FakeClientTransport` in tests), runs the v8 hello handshake (checking the server id), and offers blocking `request` with cancellation (`AbortSignal` -> `cancel` message), `serviceCatalogue`, `subscribeService`/`start`/`dispose` (decoded snapshots and updates, held until `start`), the attachment the server reports, and a disconnect listener. Listeners run in order on a delivery thread, so they may call back into the client. A protocol violation or lost transport disconnects it and fails pending requests. `serve_application_test` drives a real `pi serve` (both session backends) through it: catalogue, create, attach, subscribe to the transcript, prompt, wait.

## Durable storage (partial port of `packages/durable`)

`IStorage` (`interfaces/durable/i_storage`) is the atomic persistence boundary of the durable session: conversations, entries, tasks, submissions and versioned documents, with fork-aware entry scans, opaque cursors and as-of document reads. Records and writes are JSON in the shape of `packages/durable/src/types.ts`. Three backends exist:

- `MemoryStorage` (`src/durable/memory_storage`): the reference implementation; also an `IStagedStorage` (validate/apply in two steps).
- `JsonlStorage` (`src/durable/jsonl_storage`): `main.jsonl` commit markers plus `doc-<id>.jsonl` / `task-<id>.jsonl` sidecars, rebuilt into a memory store on open, with torn-tail and unconfirmed-record recovery and sidecar reclamation. Same on-disk format as the TS implementation (format 1).

- `SqliteStorage` (`src/durable/sqlite_storage`) over `ISqlDatabase` (`interfaces/durable/i_sql_database`; `SqliteDatabase` in `src/durable/sqlite_database` binds the system libsqlite3, WAL journal, immediate transactions). The schema and row encoding are those of the TS SQLite storage (`SqliteMigrations`), so the two implementations open each other's `session.sqlite` (cross-checked by `tools/ts_crosscheck/run.sh`). Reads are SQL queries, not an in-memory copy.

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

Differences from the TypeScript implementation: codemode and `tool_search` are not ported, so every exposure except `hidden` offers tools directly; server log files and Client ID Metadata Documents are not ported.

OAuth: HTTP servers without an `Authorization` header or `auth.provider` authenticate with the token a sign-in stored in `<agent-dir>/mcp-auth.json` (sign in with `pi mcp login` or the TypeScript CLI's `/mcp`; the file format, keys and refresh lock files are shared). `McpOauthTokenProvider` sends the stored access token, refreshes it when it is within 30 seconds of expiry or after a 401 (`McpOauthRefresher`: cached or discovered authorization server metadata with issuer validation, a `refresh_token` grant with the stored or configured client, `resource` indicator, client authentication as the server's metadata allows), saves rotated tokens under the refresh lock, and retries the request once. A rejected grant, a missing refresh token or an `insufficient_scope` challenge puts the server in `needs-auth`; the `oauth` settings `clientId`, `clientSecret` and `authServerMetadataUrl` are honored. `pi mcp login <server>` signs in (`McpOauthSignIn`): it refreshes a stored grant when it can, else discovers the authorization server, registers a client dynamically (or uses `oauth.clientId`/`clientSecret`), prints the authorization URL, waits for the browser at a loopback callback (`oauth.callbackPort`/`callbackUrl`, default `http://127.0.0.1:<free port>/callback`; `PosixCallbackServer`), checks the issuer of the response (RFC 9207) and exchanges the code with PKCE S256; `pi mcp logout <server>` forgets the credentials and `pi mcp list` shows how each server authenticates. Pasting a redirect URL (SSH) is not supported: forward the callback port instead. URL comparison uses a simplified normalization (`UrlParser`: lower-case host, default port and empty path), which matches WHATWG URL for ordinary URLs.

Resources: while a visible server announces the resources capability, three tools are registered next to its tools, with Codex's names so models use them unchanged: `list_mcp_resources` and `list_mcp_resource_templates` (`server`, `cursor`: one page of one server, or every page of every server with per-server `errors`) and `read_mcp_resource` (`server`, `uri`: text and images for the model, binary resources in temp files). MCP App resources (`ui://`, `profile=mcp-app`), `_meta` and icons are left out of listings. They are withdrawn when no such server remains.

## Provider sign-in (`pi auth`)

`pi auth list | status | login <provider> | logout <provider>` signs in to the subscription providers and stores the credential in `<agent-dir>/auth.json`, in the shape the providers' refresh keeps (the TypeScript CLI reads and refreshes the same entries). `ProviderLogin` (support) dispatches to two generic engines driven by descriptions (`ProviderLoginSpecs`, constants from `packages/ai/src/auth/oauth`): `OauthBrowserLogin` (authorization code with PKCE S256, a loopback callback server on `127.0.0.1`, or a pasted code or redirect URL when `--manual` is given or a port shared with other tools is taken; token request through `OauthTokenMapper`, the same mapping the refresh uses) and `OauthDeviceLogin` (RFC 8628 with the polling rules of `device-code.ts`: 5 s default, `slow_down` +5 s, expiry, clock-drift hint). Methods: Anthropic `browser` (default) or `copy_code`; OpenAI Codex `browser` or `device_code` (own endpoints, handled in `ProviderLogin`); Sign in with ChatGPT `browser` (dynamic client id from the callback, `id_token` and `chatgpt.tokens.use.direct` required, a random device id like the TS CLI); OpenRouter `browser` (the code is exchanged for an API key); xAI and Kimi Code `device_code`; GitHub Copilot `device_code` for github.com (the GitHub token is traded for a Copilot token through the provider's flow). Plugins that register an OAuth sign-in for a provider (`register_oauth`, see docs/cpp-plugins.md) are loaded for `list` and `login` and their providers sign in with method `plugin`. Not ported: the Meta and Radius sign-ins, GitHub Enterprise domains and the Copilot model enablement after login. `--method` picks a method and `--manual` forces the paste prompt on stdin. The provider endpoints are fixed, so there is no offline end-to-end test; the engines are tested against scripted HTTP and callback servers, and `pi auth login <provider> --manual` was run against the binary for the CLI plumbing.

## Packages (`pi install`, `remove`, `update`, `list`)

A package is a directory (a git checkout or a local path) that offers skills, prompt templates and plugins. `pi install <source> [-l]` fetches it and adds it to the `packages` list of the global settings (`-l`: the project's settings, only for a trusted project); `pi remove <source> [-l]` deletes a git checkout and drops the entry; `pi update [source]` moves git packages to the newest commit of their upstream branch (pinned ones stay); `pi list` shows the entries and where each is installed. Sources: `git:github.com/user/repo[@ref]` (also `git:user/repo`, `git:git@host:user/repo`, `github:user/repo`), `https://`, `http://`, `ssh://` and `git://` URLs, and local paths. npm sources (`npm:`) are refused: there is no JavaScript runtime. `PackageManager` (support, over `IProcessRunner` running `git`, `IFileSystem` and `ISettingsManager`) ports the git and settings half of `core/package-manager.ts`:

- Git packages are cloned to `<agent-dir>/git/<host>/<path>` (project: `<cwd>/.pi/git/...` with a `.gitignore` that keeps them out of the repository). `install` runs `git clone` and `git checkout <ref>`; `update` fetches the ref (`FETCH_HEAD`) or the upstream branch (falling back to `origin/HEAD`), and when the commit differs runs `git reset --hard` and `git clean -fdx`, so the checkout stays pristine. A failed clone is removed together with its now empty parent directories. `GIT_TERMINAL_PROMPT=0` keeps git from waiting for credentials. Hosts and paths that could leave the install directory (`..`, backslashes, encoded forms) are refused (`GitSourceParser`).
- Local packages are used in place; settings store the path relative to the scope's base directory (the agent directory, or `<cwd>/.pi`). `remove` never deletes them.
- Entries are matched by identity (`npm:<name>`, `git:<host>/<path>`, `local:<absolute path>`), so `git:github.com/a/b@v2` finds the entry written without the ref. A package named in both scopes is used from the project.
- An entry is a string or `{source, autoload?, skills?, prompts?, plugins?}`. The manifest is the `pi` object of the package's `package.json` (`skills`, `prompts`, `plugins`: files, directories and globs, with `!`, `+` and `-` entries), or else the `skills/`, `prompts/` and `plugins/` directories. Skills are `SKILL.md` files (and `.md` files directly in the skills root), prompt templates `.md` files and plugins `.so`/`.dylib` files. A settings filter list per resource type narrows the set: absent keeps everything, `[]` nothing, entries are glob includes, `!pattern` excludes, `+path` force-includes and `-path` force-excludes an exact path (`PackagePatterns`). `autoload: false` in a project entry makes it a delta over the user entry of the same package: only the paths its patterns name change.
- At startup (`CodingSessionHandle`, the durable serve resource loader and `DurableToolbox`) the enabled files join the skill, prompt template and plugin lists; missing git packages are cloned first (like the TS CLI) and a package that fails to load becomes a diagnostic. `--no-skills`, `--no-prompt-templates` and `--no-plugins` leave the package files out too.

Not ported: npm packages, themes and JavaScript extensions as package resources, `.gitignore`/`.ignore` rules while collecting package files, the offline-mode and temporary (`-e`) package sources, update availability checks and progress callbacks. The git flow is tested against a real local repository (`app/package_command`: the process inherits `GIT_CONFIG_*` redirecting a fake host to a directory) and the rest against a scripted `git`.

## Telemetry and diagnostics

- **Telemetry contracts** (`interfaces/telemetry`, port of `packages/telemetry`): `ITelemetryContext` starts spans around a callback and `ITelemetrySpan` records events, attributes and a status; spans never fail and calls on a settled span are ignored. `NoopTelemetryContext` is the default, `InMemoryTelemetryContext` the recording reference (span ids in start order, end sequence in settlement order), `SpanContext` adapts a span to a context, `TypedSpanStarter` nests spans, and `TelemetrySchemaValidator` checks span data against the serializable schema form of `defineTelemetrySchema` (TypeScript only types it). Callbacks return `Result<void>` where TypeScript rejects; an error becomes the span's error status unless the callback set a status. A span reference is valid inside its callback only. `StreamOptions` carries an optional `telemetryContext` (passed along, as in TypeScript, the providers emit no spans). Install telemetry (`enableInstallTelemetry`, overridden by `PI_TELEMETRY`) only controls provider attribution headers (`ProviderAttribution`: OpenRouter, NVIDIA NIM, Cloudflare, opencode session headers); pi sends no other telemetry.
- **Request headers**: `StreamOptions::transformHeaders` runs last over the assembled headers (the model runtime applies it); `AgentSession` uses it for the attribution headers and the `before_provider_headers` plugin event (handlers answer an object of headers to set, `null` removes one). Requests carry `User-Agent: pi (<platform> <release>; <arch>)` unless a model, provider or caller header says otherwise.
- **Prompt-cache accounting and warming**: `CacheStats` finds the tokens a turn re-billed because they were not read from the cache (port of `cache-stats.ts`; misses are keyed by entry id). `CacheWarmer` resends the cached request with a one-token cap before the entry expires (settings `cacheWarming`: `off`, `streaming` (default) or `idle`; `PI_CACHE_RETENTION`), prices each refresh and sends it only when it is expected to save at least $0.05 (or a `cache_warming_decision` plugin answer says `warm`), records `cache_warm` usage entries and stops at the one-hour (streaming) and 30-minute (idle) safety limits. `AgentSession::cacheWarmingStatus()` and `setCacheWarmingMode()` expose it; it needs the session config's `environment`. A timer thread per scheduled refresh replaces the JavaScript timer.
- **Bug reports**: the `bug_report` RPC command (`AgentSession::reportBug`) files `{hint?, includeSession?, includeSummary?, delivery: "upload" | "zip", outputPath?}`. The bundle holds `report.json` (version, host, shell and terminal facts, names of `PI_*` variables, model and provider description, loaded plugins, settings with `trackingId`/`deviceId` removed and secrets redacted by `BugReportRedactor`), `diagnostics.json` (failed assistant turns and recorded crashes, no conversation content) and optionally `session.jsonl` (the current branch, `SessionBranchSerializer`) or `summary.md` (written by the session model from the last messages that fit 60% of its window). `upload` posts multipart form data to `<gateway>/v1/bug-reports` (`PI_RADIUS_GATEWAY`, default `https://radius.pi.dev`) with the signed-in Radius token when there is one and is refused while `PI_OFFLINE` is set; `zip` writes `pi-bug-report-<id>.zip` (entries stored, not deflated) to `outputPath` or the working directory. A filed report is recorded as a `pi.bug-report` session entry and clears the crash log. The `pi.share` trailing entry of the interactive share flow is not added.
- **Crash log**: `CrashRecorder` (app) records uncaught `std::terminate` and the fatal signals SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT, with a backtrace, in `<agent dir>/crashes.json` (five newest, `CrashLog`); `pi rpc` and `pi serve` print a notice about the newest unannounced crash from the last seven days at startup. Recording is best effort and not async-signal-safe.
- **Startup timings**: `PI_TIMING=1` prints the startup marks of `pi rpc` and `pi serve` to stderr (`Timings`).

Not carried over: `output-guard.ts` (taking over a JavaScript process's stdout; the C++ rpc process writes its protocol to stdout only by construction), the stack-trace-to-extension matching of `crash-log.ts`, and `footer-data-provider.ts` (terminal footer data).

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

plugin commands and UI APIs, plugin provider OAuth; the Meta and Radius sign-ins; the compaction observations, tree events and the `before_agent_start` system prompt replacement of plugins in durable sessions (see docs/cpp-plugins.md); HTML export.
