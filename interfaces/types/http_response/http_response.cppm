export module pi.types.http_response;

import std;
export import pi.types.http_headers;

export struct HttpResponse {
    int status = 0;
    HttpHeaders headers;
    std::string body;
};
