#pragma once

#include <string>

#include "interfaces/types/http_headers/http_headers.h"

/** A request received by FakeHttpServer. */
struct FakeHttpRequest {
    std::string method;
    std::string path;
    HttpHeaders headers;
    std::string body;
};
