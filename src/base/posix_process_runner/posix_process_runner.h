#pragma once

#include <spawn.h>

#include <chrono>
#include <string>
#include <vector>

#include "interfaces/platform/i_process_runner/i_process_runner.h"

/** IProcessRunner using posix_spawn with a dedicated process group per child. */
class PosixProcessRunner : public IProcessRunner {
public:
    Result<ProcessResult> run(const ProcessRequest& request) override;

private:
    static constexpr int kKillGraceMs = 2000;

    std::vector<std::string> buildEnvironment(const ProcessRequest& request) const;
    Result<pid_t> spawn(const ProcessRequest& request, int stdinRead, int outputWrite) const;
    void pump(const ProcessRequest& request, pid_t pid, int stdinWrite, int outputRead, int wakeRead,
              ProcessResult& result) const;
    void terminate(pid_t pid, int signal) const;
    void decodeStatus(int status, ProcessResult& result) const;
};
