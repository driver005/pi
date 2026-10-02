export module pi.types.credential_info;

import std;
export import pi.types.credential;

/** Non-secret credential metadata for status listings. */
export struct CredentialInfo {
    std::string providerId;
    CredentialType type = CredentialType::ApiKey;
};
