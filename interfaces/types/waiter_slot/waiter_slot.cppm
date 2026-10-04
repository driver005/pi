export module pi.types.waiter_slot;

import std;
export import pi.support.wait_gate;
export import pi.types.json;
export import pi.types.result;

/** One pending wait: a gate that opens when the wait is resolved or rejected, and its outcome. */
export struct WaiterSlot {
    WaitGate gate;
    std::mutex mutex;
    std::optional<Result<Json>> outcome;
};
