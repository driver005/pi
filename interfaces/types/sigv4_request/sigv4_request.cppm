export module pi.types.sigv4_request;

import std;
export import pi.types.aws_credentials;
export import pi.types.http_headers;

/** What an AWS Signature Version 4 signature covers. */
export struct Sigv4Request {
    std::string method = "POST";
    std::string host;
    /** The request path exactly as sent (already percent-encoded once). */
    std::string path = "/";
    /** Canonical-ready query string: "name=value&..." with encoded names and values; may be empty. */
    std::string query;
    /** Headers covered by the signature, besides host and x-amz-date which are added. */
    HttpHeaders headers;
    std::string body;
    std::string region;
    std::string service;
    AwsCredentials credentials;
};
