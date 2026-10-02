export module pi.types.output_snapshot;

import std;
export import pi.types.truncation_result;

/** Display view of streamed command output: the tail plus bookkeeping about what was cut. */
export struct OutputSnapshot {
    std::string content;
    TruncationResult truncation;
    /** Set once the full output had to be persisted to disk. */
    std::optional<std::string> fullOutputPath;
};
