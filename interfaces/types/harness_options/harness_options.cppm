module;

#include <cstdint>

export module pi.types.harness_options;

import std;
export import pi.durable.i_execution_env;
export import pi.durable.i_registry_reader;
export import pi.provider.i_model_runtime;
export import pi.support.transaction;
export import pi.types.error;
export import pi.types.harness_run_settings;
export import pi.types.json;
export import pi.types.result;

/** What a harness is built from. */
export struct HarnessOptions {
    /** Model access used by generation, compaction and summarization. */
    IModelRuntime* models = nullptr;
    /** The registry of extensions; it may keep changing while the harness runs. */
    IRegistryReader* registry = nullptr;
    /** The run policy, read at every resolution; absent: all defaults. */
    std::function<HarnessRunSettings()> settings;
    /** Builds a conversation's environment at each use (never on the session line); absent: no environment. */
    std::function<Result<std::shared_ptr<IExecutionEnv>>(std::int64_t conversationId, const std::optional<std::string>& cwd)> env;
    /**
     * Runs in every commit that creates or forks a conversation, raw `createConversation` included, after the built-in
     * `pi.*` documents and before the conveniences apply `agent` and run `init`. A failure fails the creating commit.
     */
    std::function<Result<void>(Transaction&, const Json& conversation)> conversationCreated;
    /** The harness clock in milliseconds; default: the system clock. */
    std::function<std::int64_t()> now;
    /** Receives extension failures that do not fail the calling operation; must not throw. */
    std::function<void(const Error&)> onReport;
};
