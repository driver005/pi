export module pi.types.gate_state;

import std;

/** The shared state of a wait gate; held by shared_ptr so abort listeners may outlive the waiting thread. */
export struct GateState {
    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
};
