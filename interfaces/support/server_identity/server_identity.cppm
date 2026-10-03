export module pi.support.server_identity;

import std;
export import pi.platform.i_crypto;
export import pi.platform.i_file_system;
export import pi.types.result;

/**
 * The stable logical identity of a server: a canonical lowercase UUIDv4, given explicitly or kept in
 * the `default-server-id` file of the server directory (created on first use, private to the owner).
 * Port of acquireServerProfile's identity handling in experimental/server.ts.
 */
export class ServerIdentity {
public:
    static constexpr std::string_view kDefaultFile = "default-server-id";

    ServerIdentity(IFileSystem& files, const ICrypto& crypto)
        : m_files(files),
          m_crypto(crypto) {}

    /** `requested` wins when given (it must be valid); otherwise the directory's default identity. */
    Result<std::string> resolve(const std::string& directory, const std::optional<std::string>& requested) {
        if (requested) {
            if (!valid(*requested)) {
                return std::unexpected(Error{"invalid_server_id", "Invalid server ID: " + *requested});
            }
            return *requested;
        }
        if (auto made = m_files.createPrivateDirectories(directory); !made) {
            return std::unexpected(made.error());
        }
        const std::string path = directory + "/" + std::string(kDefaultFile);
        if (m_files.exists(path)) {
            auto content = m_files.readFile(path);
            if (!content) {
                return std::unexpected(content.error());
            }
            std::string id = *content;
            while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back()))) {
                id.pop_back();
            }
            if (!valid(id)) {
                return std::unexpected(Error{"invalid_server_id", "Invalid default server identity in " + path});
            }
            return id;
        }
        const std::string id = generate();
        if (auto written = m_files.writeFilePrivate(path, id); !written) {
            return std::unexpected(written.error());
        }
        return id;
    }

    bool valid(const std::string& id) const {
        if (id.size() != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-') {
            return false;
        }
        return hexRun(id, 0, 8) && hexRun(id, 9, 4) && id[14] == '4' && hexRun(id, 15, 3) &&
               (id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b') && hexRun(id, 20, 3) &&
               hexRun(id, 24, 12);
    }

    std::string generate() const {
        std::string bytes = m_crypto.randomBytes(16);
        bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0f) | 0x40);
        bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3f) | 0x80);
        std::string text;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                text.push_back('-');
            }
            text += std::format("{:02x}", static_cast<unsigned char>(bytes[i]));
        }
        return text;
    }

private:
    bool hexRun(const std::string& text, std::size_t from, std::size_t count) const {
        for (std::size_t i = from; i < from + count; ++i) {
            const char c = text[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    IFileSystem& m_files;
    const ICrypto& m_crypto;
};
