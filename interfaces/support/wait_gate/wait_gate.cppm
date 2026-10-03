export module pi.support.wait_gate;

import std;
export import pi.support.abort_signal;
export import pi.types.gate_state;
export import pi.types.result;

/** A one-shot latch: threads block in `wait` until `open`, or until one of the supplied signals aborts. */
export class WaitGate {
public:
    void open() {
        {
            const std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->opened = true;
        }
        m_state->cv.notify_all();
    }

    bool isOpen() const {
        const std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->opened;
    }

    /** Blocks until the gate opens (success) or any signal aborts (error "aborted"). */
    Result<void> wait(const std::vector<const AbortSignal*>& cancels = {}) const {
        std::vector<std::pair<const AbortSignal*, std::uint64_t>> registered;
        const std::shared_ptr<GateState> state = m_state;
        for (const AbortSignal* cancel : cancels) {
            if (cancel != nullptr) {
                registered.emplace_back(cancel, cancel->onAbort([state] {
                    { const std::lock_guard<std::mutex> lock(state->mutex); }
                    state->cv.notify_all();
                }));
            }
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&] { return state->opened || anyAborted(cancels); });
        const bool opened = state->opened;
        lock.unlock();
        for (const auto& entry : registered) {
            entry.first->removeListener(entry.second);
        }
        if (opened) {
            return {};
        }
        return std::unexpected(Error{"aborted", "The operation was aborted"});
    }

    /** Like `wait`, but gives up after `timeout`: true when the gate opened, false on timeout. */
    Result<bool> waitFor(std::chrono::milliseconds timeout, const std::vector<const AbortSignal*>& cancels = {}) const {
        std::vector<std::pair<const AbortSignal*, std::uint64_t>> registered;
        const std::shared_ptr<GateState> state = m_state;
        for (const AbortSignal* cancel : cancels) {
            if (cancel != nullptr) {
                registered.emplace_back(cancel, cancel->onAbort([state] {
                    { const std::lock_guard<std::mutex> lock(state->mutex); }
                    state->cv.notify_all();
                }));
            }
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, timeout, [&] { return state->opened || anyAborted(cancels); });
        const bool opened = state->opened;
        lock.unlock();
        for (const auto& entry : registered) {
            entry.first->removeListener(entry.second);
        }
        if (opened) {
            return true;
        }
        if (anyAborted(cancels)) {
            return std::unexpected(Error{"aborted", "The operation was aborted"});
        }
        return false;
    }

private:
    bool anyAborted(const std::vector<const AbortSignal*>& cancels) const {
        for (const AbortSignal* cancel : cancels) {
            if (cancel != nullptr && cancel->aborted()) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<GateState> m_state = std::make_shared<GateState>();
};
