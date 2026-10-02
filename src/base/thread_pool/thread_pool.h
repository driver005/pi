#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "interfaces/platform/i_executor/i_executor.h"

/** Fixed-size worker pool. The destructor drains queued tasks, then joins the workers. */
class ThreadPool : public IExecutor {
public:
    explicit ThreadPool(std::size_t workers);
    ~ThreadPool() override;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task) override;

private:
    void workerLoop();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};
