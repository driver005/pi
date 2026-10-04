export module pi.platform.i_executor;

import std;

/** Runs tasks on background threads. */
export class IExecutor {
public:
    virtual ~IExecutor() = default;

    virtual void submit(std::function<void()> task) = 0;
};
