export module pi.types.jsonl_storage_options;

/** How a JSONL storage persists. */
export struct JsonlStorageOptions {
    /** Flush every affected sidecar before appending the main marker (and flush the marker before reclaiming). */
    bool fsync = false;
};
