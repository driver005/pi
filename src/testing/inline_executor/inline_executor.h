#pragma once

#include <functional>

#include "interfaces/platform/i_executor/i_executor.h"

/** IExecutor that runs each task synchronously on the submitting thread (deterministic tests). */
class InlineExecutor : public IExecutor {
public:
    void submit(std::function<void()> task) override {
        task();
    }
};
