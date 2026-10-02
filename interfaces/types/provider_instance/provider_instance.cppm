module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.provider_instance;

import std;
export import pi.chord.i_remote_service;
export import pi.types.json;

/** A live service implementation inside the provider: its address (keyed only) and wiring. */
export struct ProviderInstance {
    /** `{"key", "generation"}` for keyed instances, null for a singleton. */
    Json address;
    std::shared_ptr<IRemoteService> implementation;
    std::map<std::string, IRemoteService::Method> methods;
    std::map<std::string, IReplicatedState*> states;
    /** State listeners the provider registered, removed again when the instance goes away. */
    std::vector<std::pair<IReplicatedState*, std::uint64_t>> stateListeners;
    std::atomic<bool> active{true};
};
