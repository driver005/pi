export module pi.support.mcp_tool_namer;

import std;
export import pi.platform.i_crypto;

/**
 * Model-facing names of MCP tools: `mcp__<server>__<tool>` with everything but `[A-Za-z0-9_]`
 * replaced by `_`, at most 64 characters, shortened with an 8-hex-digit hash suffix when too long
 * or when sanitizing made two tools collide. Port of createMcpToolName in
 * packages/coding-agent/src/extensions/mcp/tools.ts.
 */
export class McpToolNamer {
public:
    static constexpr std::size_t kMaxNameLength = 64;

    explicit McpToolNamer(const ICrypto& crypto)
        : m_crypto(crypto) {}

    /** `isTaken` reports names already used by a different MCP tool. */
    std::string create(const std::string& server, const std::string& tool, const std::function<bool(const std::string&)>& isTaken = {}) const {
        const std::string name = sanitize("mcp__" + server + "__" + tool);
        if (name.size() <= kMaxNameLength && !(isTaken && isTaken(name))) {
            return name;
        }
        const std::string hash = hashSuffix(server, tool);
        return name.substr(0, std::min(name.size(), kMaxNameLength - hash.size() - 1)) + "_" + hash;
    }

private:
    std::string sanitize(const std::string& text) const {
        std::string out = text;
        for (char& c : out) {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
                c = '_';
            }
        }
        return out;
    }

    std::string hashSuffix(const std::string& server, const std::string& tool) const {
        const std::string digest = m_crypto.sha256(server + std::string(1, '\0') + tool);
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string hex;
        for (std::size_t i = 0; i < 4 && i < digest.size(); ++i) {
            const auto byte = static_cast<unsigned char>(digest[i]);
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 0x0F]);
        }
        return hex;
    }

    const ICrypto& m_crypto;
};
