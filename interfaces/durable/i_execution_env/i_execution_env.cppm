export module pi.durable.i_execution_env;

import std;

/** The environment a conversation's tools run in: its working directory and, through the composition root, its OS access. */
export class IExecutionEnv {
public:
    virtual ~IExecutionEnv() = default;

    virtual std::string cwd() const = 0;
};
