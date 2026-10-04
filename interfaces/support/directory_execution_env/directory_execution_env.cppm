export module pi.support.directory_execution_env;

import std;
export import pi.durable.i_execution_env;

/** The execution environment of a conversation working in one directory. */
export class DirectoryExecutionEnv : public IExecutionEnv {
public:
    explicit DirectoryExecutionEnv(std::string cwd)
        : m_cwd(std::move(cwd)) {}

    std::string cwd() const override {
        return m_cwd;
    }

private:
    std::string m_cwd;
};
