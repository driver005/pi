# third_party

External dependencies are declared in `MODULE.bazel` (Bazel Central Registry, pinned versions)
and exposed to modules only through the aliases in `BUILD.bazel`.

| alias    | module        | use                                   |
|----------|---------------|---------------------------------------|
| json     | nlohmann_json | JSON values, session/settings files   |
| curl     | curl          | HTTPS, SSE streaming                  |
| crypto   | boringssl     | SHA-256, HMAC, random (pulled by curl)|
| zstd     | zstd          | openai-codex request bodies           |
| yaml     | yaml-cpp      | skill / prompt-template frontmatter   |
| gtest    | googletest    | tests                                 |

Not available from the registry or blocked in the dev sandbox, handled in-tree instead:
JSON Schema validation (hand-written subset validator), sqlite3 (needs sqlite.org, see M6).
