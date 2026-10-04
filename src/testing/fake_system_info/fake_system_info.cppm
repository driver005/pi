export module pi.testing.fake_system_info;

import std;
export import pi.platform.i_system_info;

/** ISystemInfo with fixed answers. */
export class FakeSystemInfo : public ISystemInfo {
public:
    std::string platform() const override {
        return "linux";
    }

    std::string arch() const override {
        return "x64";
    }

    std::string osRelease() const override {
        return "6.1.0-test";
    }

    std::string osVersion() const override {
        return "#1 SMP test";
    }
};
