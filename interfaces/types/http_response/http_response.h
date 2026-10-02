#pragma once

#include <string>

#include "interfaces/types/http_headers/http_headers.h"

struct HttpResponse {
    int status = 0;
    HttpHeaders headers;
    std::string body;
};
