export module pi.types.bedrock_connection;

import std;
export import pi.types.aws_credentials;

/** Where and how a Bedrock request is sent. */
export struct BedrockConnection {
    /** Scheme and host (and any path prefix) without the trailing slash. */
    std::string endpoint;
    std::string region;
    /** Set for Bedrock API key authentication; no SigV4 signature then. */
    std::optional<std::string> bearerToken;
    /** Set for SigV4 signing. */
    std::optional<AwsCredentials> credentials;
};
