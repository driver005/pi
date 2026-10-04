export module pi.types.parsed_url;

import std;

/** An absolute http(s) URL split into its parts. The host is lower case, the port absent when it is the scheme's default. */
export struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::optional<int> port;
    /** Always starts with '/'. */
    std::string path = "/";
    /** Without the leading '?'; empty when absent. */
    std::string query;
    /** Without the leading '#'; empty when absent. */
    std::string fragment;
};
