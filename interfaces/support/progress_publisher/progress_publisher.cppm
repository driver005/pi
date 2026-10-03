module;

#include <cstdint>

export module pi.support.progress_publisher;

import std;
export import pi.types.error;
export import pi.types.result;
export import pi.types.waiter_slot;

/**
 * Adaptive progress commits, like the environment's shell output capture: the first change after an idle period
 * commits at once; each commit then delays the next by at least 100 ms and by its written size at 100 KiB/s. At most
 * one commit is in flight; changes made meanwhile coalesce into the next one. Commits run on the publisher's own
 * thread. Port of Progress in packages/durable/src/harness/output.ts.
 */
export class ProgressPublisher {
public:
    static constexpr std::int64_t kMinIntervalMs = 100;
    static constexpr std::int64_t kBytesPerSecond = 100 * 1024;

    /** `write` commits what changed and returns the bytes it wrote; `onError` receives its failures. */
    ProgressPublisher(std::function<Result<std::int64_t>()> write, std::function<void(const Error&)> onError)
        : m_write(std::move(write)), m_onError(std::move(onError)) {}

    ProgressPublisher(const ProgressPublisher&) = delete;
    ProgressPublisher& operator=(const ProgressPublisher&) = delete;

    ~ProgressPublisher() {
        for (const auto& waiter : stop()) {
            settle(waiter, std::unexpected(Error{"stopped", "Progress publisher stopped"}));
        }
    }

    /** Schedules a commit. */
    void mark() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopped) {
                return;
            }
            m_dirty = true;
            if (!m_thread.joinable()) {
                m_thread = std::thread([this] { loop(); });
            }
        }
        m_cv.notify_all();
    }

    /** Schedules a commit; the returned slot settles with the commit that includes this change. */
    std::shared_ptr<WaiterSlot> markAndWait() {
        auto waiter = std::make_shared<WaiterSlot>();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_waiters.push_back(waiter);
        }
        mark();
        return waiter;
    }

    /** Blocks until the slot settles with the commit that included its change. */
    Result<void> await(const std::shared_ptr<WaiterSlot>& waiter) const {
        (void)waiter->gate.wait();
        const std::lock_guard<std::mutex> lock(waiter->mutex);
        return waiter->outcome->has_value() ? Result<void>() : std::unexpected(waiter->outcome->error());
    }

    /** Stops committing and waits for the commit in flight; returns the waiters the final commit must settle. */
    std::vector<std::shared_ptr<WaiterSlot>> stop() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        return std::exchange(m_waiters, {});
    }

    /** Settles a waiter returned by `stop` successfully. */
    void resolve(const std::shared_ptr<WaiterSlot>& waiter) const {
        settle(waiter, Result<Json>(Json(nullptr)));
    }

    /** Settles a waiter returned by `stop` with a failure. */
    void reject(const std::shared_ptr<WaiterSlot>& waiter, const Error& error) const {
        settle(waiter, std::unexpected(error));
    }

private:
    using Clock = std::chrono::steady_clock;

    void loop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_stopped) {
            if (!m_dirty) {
                m_cv.wait(lock);
                continue;
            }
            const Clock::time_point now = Clock::now();
            if (now < m_nextAt) {
                m_cv.wait_until(lock, m_nextAt);
                continue;
            }
            m_dirty = false;
            std::vector<std::shared_ptr<WaiterSlot>> waiters = std::exchange(m_waiters, {});
            lock.unlock();
            flush(now, waiters);
            lock.lock();
        }
    }

    void flush(Clock::time_point started, const std::vector<std::shared_ptr<WaiterSlot>>& waiters) {
        Result<std::int64_t> written = m_write();
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (written) {
            const std::int64_t pause = std::max<std::int64_t>(kMinIntervalMs, *written * 1000 / kBytesPerSecond);
            m_nextAt = started + std::chrono::milliseconds(pause);
            for (const auto& waiter : waiters) {
                settle(waiter, Result<Json>(Json(nullptr)));
            }
            return;
        }
        m_nextAt = started + std::chrono::milliseconds(kMinIntervalMs);
        for (const auto& waiter : waiters) {
            settle(waiter, std::unexpected(written.error()));
        }
        m_onError(written.error());
    }

    void settle(const std::shared_ptr<WaiterSlot>& waiter, Result<Json> outcome) const {
        {
            const std::lock_guard<std::mutex> lock(waiter->mutex);
            if (waiter->outcome) {
                return;
            }
            waiter->outcome = std::move(outcome);
        }
        waiter->gate.open();
    }

    std::function<Result<std::int64_t>()> m_write;
    std::function<void(const Error&)> m_onError;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::vector<std::shared_ptr<WaiterSlot>> m_waiters;
    Clock::time_point m_nextAt{};
    bool m_dirty = false;
    bool m_stopped = false;
};
