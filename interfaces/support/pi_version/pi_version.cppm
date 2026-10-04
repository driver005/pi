export module pi.support.pi_version;

import std;

/** The version pi reports (the version of packages/coding-agent that this port follows; bump it with that package). */
export class PiVersion {
public:
    std::string value() const {
        return "1.0.0";
    }
};
