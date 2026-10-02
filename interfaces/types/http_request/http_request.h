#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "interfaces/support/abort_signal/abort_signal.h"
#include "interfaces/types/http_headers/http_headers.h"

/** One HTTP exchange. */
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    HttpHeaders headers;
    std::string body;
    /** Whole-exchange timeout; zero disables it. */
    std::chrono::milliseconds timeout{0};
    /** Abort if no bytes are received for this long (streams); zero disables it. */
    std::chrono::milliseconds idleTimeout{0};
    /** Explicit proxy URL; empty means use the standard environment variables. */
    std::string proxy;
    /**
     * When set, bodies of 2xx responses are delivered chunk by chunk to this callback and
     * HttpResponse::body stays empty. Error responses are always collected into body.
     */
    std::function<void(std::string_view)> onBody;
    std::shared_ptr<AbortSignal> signal;
};
