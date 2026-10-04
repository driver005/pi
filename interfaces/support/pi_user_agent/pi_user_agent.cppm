export module pi.support.pi_user_agent;

import std;
export import pi.platform.i_system_info;

/** The User-Agent of provider requests: `pi (<platform> <release>; <arch>)`, as packages/ai/src/utils/pi-user-agent.ts builds it. */
export class PiUserAgent {
public:
    std::string value(const ISystemInfo& system) const {
        return "pi (" + system.platform() + " " + system.osRelease() + "; " + system.arch() + ")";
    }
};
