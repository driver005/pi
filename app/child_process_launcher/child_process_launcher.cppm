export module pi.child_process_launcher;

import std;
export import pi.platform.i_child_process_launcher;
import pi.base.posix_child_process;

/** IChildProcessLauncher creating and starting PosixChildProcess instances. */
export class ChildProcessLauncher : public IChildProcessLauncher {
public:
    Result<std::unique_ptr<IChildProcess>> launch(const ProcessRequest& request) override;
};

Result<std::unique_ptr<IChildProcess>> ChildProcessLauncher::launch(const ProcessRequest& request) {
    auto child = std::make_unique<PosixChildProcess>(request);
    if (auto started = child->start(); !started) {
        return std::unexpected(started.error());
    }
    return std::unique_ptr<IChildProcess>(std::move(child));
}
