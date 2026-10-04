export module pi.support.radius_gateway;

import std;
export import pi.platform.i_environment;

/** The Radius gateway pi talks to: the default origin, the `PI_RADIUS_GATEWAY` override and URL normalization. */
export class RadiusGateway {
public:
    std::string providerId() const {
        return "radius";
    }

    std::string defaultGateway() const {
        return "https://radius.pi.dev";
    }

    /** Adds `https://` when there is no scheme and drops trailing slashes. */
    std::string normalize(const std::string& value) const {
        std::string lower = value.substr(0, 8);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string out = lower.starts_with("http://") || lower.starts_with("https://") ? value : "https://" + value;
        while (!out.empty() && out.back() == '/') {
            out.pop_back();
        }
        return out;
    }

    /** The gateway origin, honoring the override. */
    std::string gatewayUrl(const IEnvironment& environment) const {
        return normalize(environment.get("PI_RADIUS_GATEWAY").value_or(defaultGateway()));
    }

    /** The MCP endpoint of the default gateway, which the built-in Radius provider signs in to. */
    std::string mcpUrl() const {
        return normalize(defaultGateway()) + "/mcp";
    }
};
