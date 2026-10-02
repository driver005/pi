export module pi.types.aws_credentials;

import std;

/** Static AWS credentials; sessionToken is set for temporary credentials. */
export struct AwsCredentials {
    std::string accessKeyId;
    std::string secretAccessKey;
    std::optional<std::string> sessionToken;
};
