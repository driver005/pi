export module pi.platform.i_system_info;

import std;

/** What the host machine is, for user agents and bug reports. */
export class ISystemInfo {
public:
    virtual ~ISystemInfo() = default;

    /** "linux", "darwin", ... (the names Node's os.platform() uses). */
    virtual std::string platform() const = 0;
    /** "x64", "arm64", ... (Node's os.arch() names). */
    virtual std::string arch() const = 0;
    /** The kernel release, e.g. "6.8.0-45-generic". */
    virtual std::string osRelease() const = 0;
    /** The kernel version string. */
    virtual std::string osVersion() const = 0;
};
