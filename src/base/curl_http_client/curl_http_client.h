#pragma once

#include <string>

#include "interfaces/platform/i_http_client/i_http_client.h"

/** IHttpClient on libcurl (multi interface, so aborts and idle timeouts are prompt). */
class CurlHttpClient : public IHttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;

    Result<HttpResponse> send(const HttpRequest& request) override;

private:
    std::string lowercase(std::string text) const;
    std::string caBundlePath() const;
};
