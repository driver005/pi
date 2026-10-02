module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.provider_subscriber;

import std;
export import pi.chord.i_service_provider;
export import pi.types.json;
export import pi.types.service_context;

/** One subscription's delivery state: buffered updates until activation, drained in order. */
export struct ProviderSubscriber {
    IServiceProvider::UpdateListener listener;
    std::mutex mutex;
    std::deque<std::pair<Json, ServiceContext>> buffer;
    /** Sequence of every state at the baseline; updates it already covers are dropped. */
    std::map<std::string, std::int64_t> snapshotSequences;
    bool active = false;
    bool draining = false;
    bool terminated = false;
    bool closed = false;
};
