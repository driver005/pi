#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "interfaces/support/abort_signal/abort_signal.h"

/** What to run. stdout and stderr are merged into one stream. */
struct ProcessRequest {
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> cwd;
    /** Variables layered over (or, with inheritEnv=false, instead of) the parent environment. */
    std::map<std::string, std::string> env;
    bool inheritEnv = true;
    std::string stdinData;
    /** Zero means no timeout. */
    std::chrono::milliseconds timeout{0};
    /** Called with each output chunk as it arrives (from the calling thread). */
    std::function<void(std::string_view)> onOutput;
    /** Keep the full output in ProcessResult::output (disable for very chatty commands). */
    bool captureOutput = true;
    std::shared_ptr<AbortSignal> signal;
};
