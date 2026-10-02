export module pi.testing.inline_executor;

import std;
export import pi.platform.i_executor;

/** IExecutor that runs each task synchronously on the submitting thread (deterministic tests). */
export class InlineExecutor : public IExecutor {
public:
    void submit(std::function<void()> task) override {
        task();
    }
};
