module;

#include <sys/utsname.h>

export module pi.base.posix_system_info;

import std;
export import pi.platform.i_system_info;

/** ISystemInfo over uname(2). */
export class PosixSystemInfo : public ISystemInfo {
public:
    PosixSystemInfo() {
        utsname info{};
        if (uname(&info) == 0) {
            m_platform = info.sysname;
            std::ranges::transform(m_platform, m_platform.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            m_release = info.release;
            m_version = info.version;
            m_arch = info.machine;
        }
        if (m_arch == "x86_64") {
            m_arch = "x64";
        } else if (m_arch == "aarch64") {
            m_arch = "arm64";
        }
    }

    std::string platform() const override {
        return m_platform;
    }

    std::string arch() const override {
        return m_arch;
    }

    std::string osRelease() const override {
        return m_release;
    }

    std::string osVersion() const override {
        return m_version;
    }

private:
    std::string m_platform = "unknown";
    std::string m_arch = "unknown";
    std::string m_release;
    std::string m_version;
};
