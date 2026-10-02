export module pi.base.thread_sleeper;

import std;
import pi.platform.i_sleeper;

/** ISleeper that blocks the calling thread on a condition variable until time or abort. */
export class ThreadSleeper : public ISleeper {
public:
    bool sleep(std::chrono::milliseconds duration,
               const std::shared_ptr<AbortSignal>& signal) override;
};

bool ThreadSleeper::sleep(std::chrono::milliseconds duration,
                          const std::shared_ptr<AbortSignal>& signal) {
    if (!signal) {
        std::this_thread::sleep_for(duration);
        return true;
    }
    std::mutex mutex;
    std::condition_variable wake;
    bool woken = false;
    const std::uint64_t id = signal->onAbort([&]() {
        const std::lock_guard<std::mutex> lock(mutex);
        woken = true;
        wake.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, duration, [&]() { return woken; });
    }
    signal->removeListener(id);
    return !signal->aborted();
}
