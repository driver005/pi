export module pi.testing.fake_http_request;

import std;
export import pi.types.http_headers;

/** A request received by FakeHttpServer. */
export struct FakeHttpRequest {
    std::string method;
    std::string path;
    HttpHeaders headers;
    std::string body;
};
