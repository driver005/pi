module;

#include <cstdint>

export module pi.types.jsonl_recovery_state;

import std;
export import pi.types.json;
export import pi.types.jsonl_file;

/** What recovery learns about the files of a JSONL storage while it replays them. */
export struct JsonlRecoveryState {
    JsonlFile main;
    std::map<std::string, JsonlFile> sidecarFiles;
    /** Sidecar records by `[file, seq, ordinal]`. */
    std::map<std::string, Json> recordByKey;
    std::set<std::int64_t> currentOnlyDocuments;
    std::set<std::int64_t> retiredCurrentOnlyDocuments;
    std::map<std::int64_t, bool> finalTaskIsLive;
    std::set<std::int64_t> terminalTasks;
    /** Newest base record of every current-only document. */
    std::map<std::int64_t, Json> latestBases;
    std::set<std::string> confirmed;
};
