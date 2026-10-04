module;

#include <cstdint>

export module pi.support.aws_sigv4_signer;

import std;
export import pi.platform.i_crypto;
export import pi.types.http_headers;
export import pi.types.sigv4_request;

/**
 * AWS Signature Version 4 (header based). Returns the headers to add to the request:
 * x-amz-date, x-amz-security-token (temporary credentials) and Authorization. Every header in
 * the request plus host and x-amz-date is signed. Paths are encoded a second time for the
 * canonical request, as every service except S3 requires.
 */
export class AwsSigv4Signer {
public:
    explicit AwsSigv4Signer(const ICrypto& crypto)
        : m_crypto(crypto) {}

    HttpHeaders sign(const Sigv4Request& request, std::int64_t nowMs) const {
        const std::string stamp = amzDate(nowMs);
        const std::string date = stamp.substr(0, 8);
        std::map<std::string, std::string> signed_;
        for (const auto& header : request.headers) {
            signed_[lower(header.first)] = trimValue(header.second);
        }
        signed_["host"] = request.host;
        signed_["x-amz-date"] = stamp;
        if (request.credentials.sessionToken) {
            signed_["x-amz-security-token"] = *request.credentials.sessionToken;
        }
        std::string canonicalHeaders;
        std::string names;
        for (const auto& entry : signed_) {
            canonicalHeaders += entry.first + ":" + entry.second + "\n";
            names += (names.empty() ? "" : ";") + entry.first;
        }
        const std::string canonical = request.method + "\n" + canonicalPath(request.path) + "\n" + request.query + "\n" +
                                      canonicalHeaders + "\n" + names + "\n" + hex(m_crypto.sha256(request.body));
        const std::string scope = date + "/" + request.region + "/" + request.service + "/aws4_request";
        const std::string toSign = "AWS4-HMAC-SHA256\n" + stamp + "\n" + scope + "\n" + hex(m_crypto.sha256(canonical));
        const std::string signature =
            hex(m_crypto.hmacSha256(signingKey(request.credentials, date, request.region, request.service), toSign));
        HttpHeaders out = {{"x-amz-date", stamp}};
        if (request.credentials.sessionToken) {
            out.emplace_back("x-amz-security-token", *request.credentials.sessionToken);
        }
        out.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + request.credentials.accessKeyId + "/" + scope +
                                              ", SignedHeaders=" + names + ", Signature=" + signature);
        return out;
    }

    /** Percent-encodes everything except unreserved characters (and '/' when keepSlash). */
    std::string encode(const std::string& text, bool keepSlash) const {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out;
        for (const char c : text) {
            const bool unreserved = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.' ||
                                    c == '~' || (keepSlash && c == '/');
            if (unreserved) {
                out.push_back(c);
            } else {
                out.push_back('%');
                out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
            }
        }
        return out;
    }

private:
    std::string hex(const std::string& bytes) const {
        constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const unsigned char c : bytes) {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
        return out;
    }

    std::string amzDate(std::int64_t nowMs) const {
        const std::chrono::sys_time<std::chrono::seconds> time{std::chrono::seconds{nowMs / 1000}};
        const auto days = std::chrono::floor<std::chrono::days>(time);
        const std::chrono::year_month_day date{days};
        const std::chrono::hh_mm_ss<std::chrono::seconds> clock{time - days};
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%04d%02u%02uT%02d%02d%02dZ", static_cast<int>(date.year()),
                      static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                      static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                      static_cast<int>(clock.seconds().count()));
        return buffer;
    }

    std::string canonicalPath(const std::string& path) const {
        return path.empty() ? "/" : encode(path, true);
    }

    std::string lower(const std::string& text) const {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::string trimValue(const std::string& text) const {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return "";
        }
        std::string trimmed = text.substr(first, text.find_last_not_of(" \t") - first + 1);
        std::string out;
        bool space = false;
        for (const char c : trimmed) {
            if (c == ' ' || c == '\t') {
                space = true;
                continue;
            }
            if (space && !out.empty()) {
                out.push_back(' ');
            }
            space = false;
            out.push_back(c);
        }
        return out;
    }

    std::string signingKey(const AwsCredentials& credentials, const std::string& date, const std::string& region, const std::string& service) const {
        const std::string dateKey = m_crypto.hmacSha256("AWS4" + credentials.secretAccessKey, date);
        const std::string regionKey = m_crypto.hmacSha256(dateKey, region);
        const std::string serviceKey = m_crypto.hmacSha256(regionKey, service);
        return m_crypto.hmacSha256(serviceKey, "aws4_request");
    }

    const ICrypto& m_crypto;
};
