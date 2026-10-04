export module pi.tool.i_file_mutation_queue;

import std;

/** Serializes file mutations per file; different files proceed in parallel. */
export class IFileMutationQueue {
public:
    virtual ~IFileMutationQueue() = default;

    /**
     * Runs `action` once every earlier mutation of the same file (keyed by real path) finished.
     * Blocks the caller; the lock is held until `action` returns.
     */
    virtual void run(const std::string& path, const std::function<void()>& action) = 0;
};
