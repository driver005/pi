export module pi.support.bedrock_connection_resolver;

import std;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.support.ini_parser;
export import pi.types.bedrock_connection;
export import pi.types.model;
export import pi.types.result;

/**
 * Works out the endpoint, region and authentication of a Bedrock request. Region: the region in an
 * inference profile ARN, else AWS_REGION / AWS_DEFAULT_REGION, else the profile's region, else the
 * region of a standard endpoint, else us-east-1. Authentication: a Bedrock API key (the call's API
 * key or AWS_BEARER_TOKEN_BEDROCK), dummy credentials with AWS_BEDROCK_SKIP_AUTH=1, access keys from
 * the environment, else a profile in ~/.aws/credentials (or AWS_SHARED_CREDENTIALS_FILE) and
 * ~/.aws/config. SSO, assume-role, credential_process and instance metadata are not supported.
 */
export class BedrockConnectionResolver {
public:
    BedrockConnectionResolver(const IEnvironment& environment, IFileSystem& files)
        : m_environment(environment),
          m_files(files) {}

    Result<BedrockConnection> resolve(const Model& model, const std::optional<std::string>& apiKey, const std::map<std::string, std::string>& env) {
        BedrockConnection connection;
        const auto scopedProfile = env.find("AWS_PROFILE");
        const std::optional<std::string> explicitProfile =
            scopedProfile != env.end() && !scopedProfile->second.empty() ? std::optional<std::string>(scopedProfile->second)
                                                                         : std::nullopt;
        const auto ambientProfile = processValue("AWS_PROFILE");
        const std::string profile = explicitProfile.value_or(ambientProfile.value_or("default"));
        const auto configuredRegion = [&]() -> std::optional<std::string> {
            const auto region = envValue(env, "AWS_REGION");
            return region ? region : envValue(env, "AWS_DEFAULT_REGION");
        }();
        const auto endpointRegion = standardEndpointRegion(model.baseUrl);
        const bool explicitEndpoint = !model.baseUrl.empty() && (!endpointRegion || (!configuredRegion && !ambientProfile));
        const auto fromArn = arnRegion(model.id);
        if (fromArn) {
            connection.region = *fromArn;
        } else if (configuredRegion) {
            connection.region = *configuredRegion;
        } else if (const auto fromProfile = profileRegion(profile)) {
            connection.region = *fromProfile;
        } else if (endpointRegion && explicitEndpoint) {
            connection.region = *endpointRegion;
        } else {
            connection.region = "us-east-1";
        }
        std::string endpoint = "https://bedrock-runtime." + connection.region + ".amazonaws.com";
        if (explicitEndpoint) {
            endpoint = model.baseUrl;
            while (!endpoint.empty() && endpoint.back() == '/') {
                endpoint.pop_back();
            }
        }
        connection.endpoint = endpoint;

        const bool skipAuth = envValue(env, "AWS_BEDROCK_SKIP_AUTH") == std::optional<std::string>("1");
        std::optional<std::string> bearer = apiKey && !apiKey->empty() ? apiKey : envValue(env, "AWS_BEARER_TOKEN_BEDROCK");
        if (skipAuth) {
            connection.credentials = AwsCredentials{"dummy-access-key", "dummy-secret-key", std::nullopt};
            return connection;
        }
        if (bearer) {
            connection.bearerToken = bearer;
            return connection;
        }
        const auto keyId = envValue(env, "AWS_ACCESS_KEY_ID");
        const auto secret = envValue(env, "AWS_SECRET_ACCESS_KEY");
        if (keyId && secret && !explicitProfile) {
            connection.credentials = AwsCredentials{*keyId, *secret, envValue(env, "AWS_SESSION_TOKEN")};
            return connection;
        }
        auto credentials = profileCredentials(profile);
        if (!credentials) {
            return std::unexpected(Error{"no_credentials",
                                         credentials.error().message +
                                             "; set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, AWS_BEARER_TOKEN_BEDROCK, "
                                             "or add a profile to ~/.aws/credentials"});
        }
        connection.credentials = *credentials;
        return connection;
    }

private:
    std::optional<std::string> envValue(const std::map<std::string, std::string>& env, const std::string& name) const {
        const auto scoped = env.find(name);
        if (scoped != env.end() && !scoped->second.empty()) {
            return scoped->second;
        }
        return processValue(name);
    }

    std::optional<std::string> processValue(const std::string& name) const {
        const auto value = m_environment.get(name);
        return value && !value->empty() ? value : std::nullopt;
    }

    std::string lower(const std::string& text) const {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::optional<std::string> arnRegion(const std::string& modelId) const {
        if (!modelId.starts_with("arn:aws")) {
            return std::nullopt;
        }
        const auto service = modelId.find(":bedrock:");
        if (service == std::string::npos) {
            return std::nullopt;
        }
        const auto start = service + 9;
        const auto end = modelId.find(':', start);
        return end == std::string::npos || end == start ? std::nullopt : std::optional<std::string>(modelId.substr(start, end - start));
    }

    std::optional<std::string> standardEndpointRegion(const std::string& baseUrl) const {
        const auto scheme = baseUrl.find("://");
        if (scheme == std::string::npos) {
            return std::nullopt;
        }
        const auto end = baseUrl.find_first_of("/:?#", scheme + 3);
        const std::string host = lower(baseUrl.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3));
        for (const std::string prefix : {"bedrock-runtime.", "bedrock-runtime-fips."}) {
            if (!host.starts_with(prefix)) {
                continue;
            }
            for (const std::string suffix : {".amazonaws.com", ".amazonaws.com.cn"}) {
                if (host.ends_with(suffix) && host.size() > prefix.size() + suffix.size()) {
                    return host.substr(prefix.size(), host.size() - prefix.size() - suffix.size());
                }
            }
        }
        return std::nullopt;
    }

    std::map<std::string, IniParser::Section> readIni(const std::string& path) {
        const auto text = m_files.readFile(path);
        return text ? m_ini.parse(*text) : std::map<std::string, IniParser::Section>{};
    }

    std::optional<std::string> profileRegion(const std::string& profile) {
        const std::string path = processValue("AWS_CONFIG_FILE").value_or(homePath(".aws/config"));
        const auto sections = readIni(path);
        const std::vector<std::string> names{profile == "default" ? "default" : "profile " + profile, profile};
        for (const auto& name : names) {
            const auto section = sections.find(name);
            if (section != sections.end() && section->second.contains("region") && !section->second.at("region").empty()) {
                return section->second.at("region");
            }
        }
        return std::nullopt;
    }

    Result<AwsCredentials> profileCredentials(const std::string& profile) {
        const std::string credentialsPath = processValue("AWS_SHARED_CREDENTIALS_FILE").value_or(homePath(".aws/credentials"));
        std::vector<std::map<std::string, IniParser::Section>> files{readIni(credentialsPath)};
        files.push_back(readIni(processValue("AWS_CONFIG_FILE").value_or(homePath(".aws/config"))));
        for (const auto& sections : files) {
            const std::vector<std::string> names{profile, profile == "default" ? profile : "profile " + profile};
            for (const auto& name : names) {
                const auto section = sections.find(name);
                if (section == sections.end()) {
                    continue;
                }
                const auto key = section->second.find("aws_access_key_id");
                const auto secret = section->second.find("aws_secret_access_key");
                if (key != section->second.end() && secret != section->second.end()) {
                    AwsCredentials credentials{key->second, secret->second, std::nullopt};
                    if (const auto token = section->second.find("aws_session_token"); token != section->second.end()) {
                        credentials.sessionToken = token->second;
                    }
                    return credentials;
                }
            }
        }
        return std::unexpected(Error{"no_credentials", "Could not load AWS credentials for profile \"" + profile + "\""});
    }

    std::string homePath(const std::string& relative) const {
        return m_files.homeDirectory() + "/" + relative;
    }

    const IEnvironment& m_environment;
    IFileSystem& m_files;
    IniParser m_ini;
};
