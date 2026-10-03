export module pi.types.jsonl_encoded_commit;

import std;

/** A commit as it is appended to disk: the main marker line and the sidecar lines per file. */
export struct JsonlEncodedCommit {
    std::string marker;
    std::map<std::string, std::string> sidecars;
};
