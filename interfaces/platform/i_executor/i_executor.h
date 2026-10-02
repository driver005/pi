#pragma once

#include <functional>

/** Runs tasks on background threads. */
class IExecutor {
public:
    virtual ~IExecutor() = default;

    virtual void submit(std::function<void()> task) = 0;
};
