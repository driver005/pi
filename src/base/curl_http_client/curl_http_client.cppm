module;

#include <curl/curl.h>
#include <cstdlib>

export module pi.base.curl_http_client;

import std;
export import pi.platform.i_http_client;

/** IHttpClient on libcurl (multi interface, so aborts and idle timeouts are prompt). */
export class CurlHttpClient : public IHttpClient {
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

CurlHttpClient::CurlHttpClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlHttpClient::~CurlHttpClient() {
    curl_global_cleanup();
}

std::string CurlHttpClient::lowercase(std::string text) const {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string CurlHttpClient::caBundlePath() const {
    for (const char* name : {"CURL_CA_BUNDLE", "SSL_CERT_FILE"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return "";
}

Result<HttpResponse> CurlHttpClient::send(const HttpRequest& request) {
    using Clock = std::chrono::steady_clock;
    CURL* easy = curl_easy_init();
    CURLM* multi = curl_multi_init();
    if (easy == nullptr || multi == nullptr) {
        return std::unexpected(Error{"http_init", "failed to initialise libcurl"});
    }
    HttpResponse response;
    const bool streaming = static_cast<bool>(request.onBody);
    Clock::time_point lastActivity = Clock::now();

    std::function<std::size_t(char*, std::size_t)> onHeader = [&](char* data, std::size_t length) {
        const std::string line(data, length);
        if (line.rfind("HTTP/", 0) == 0) {
            response.headers.clear();
        } else if (line == "\r\n" || line == "\n") {
            long status = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
            if (status >= 200 && request.onResponse) {
                request.onResponse(static_cast<int>(status), response.headers);
            }
        } else {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of("\r\n") + 1);
                response.headers.emplace_back(line.substr(0, colon), value);
            }
        }
        return length;
    };
    std::function<std::size_t(char*, std::size_t)> onWrite = [&](char* data, std::size_t length) {
        lastActivity = Clock::now();
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        if (streaming && status >= 200 && status < 300) {
            request.onBody(std::string_view(data, length));
        } else {
            response.body.append(data, length);
        }
        return length;
    };
    curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    if (request.timeout.count() > 0) {
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    }
    if (!request.proxy.empty()) {
        curl_easy_setopt(easy, CURLOPT_PROXY, request.proxy.c_str());
    }
    if (const std::string bundle = caBundlePath(); !bundle.empty()) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, bundle.c_str());
    }
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
        request.method == "PATCH") {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    curl_slist* headerList = nullptr;
    for (const auto& [name, value] : request.headers) {
        headerList = curl_slist_append(headerList, (name + ": " + value).c_str());
    }
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
                     +[](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
                         return (*static_cast<decltype(onHeader)*>(user))(data, size * count);
                     });
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &onHeader);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                     +[](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
                         return (*static_cast<decltype(onWrite)*>(user))(data, size * count);
                     });
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &onWrite);

    std::atomic<bool> aborted{false};
    std::uint64_t listenerId = 0;
    if (request.signal != nullptr) {
        listenerId = request.signal->onAbort([&] {
            aborted = true;
            curl_multi_wakeup(multi);
        });
    }
    curl_multi_add_handle(multi, easy);
    bool idleTimedOut = false;
    int running = 1;
    while (running > 0 && !aborted) {
        curl_multi_perform(multi, &running);
        if (running == 0) {
            break;
        }
        curl_multi_poll(multi, nullptr, 0, 200, nullptr);
        if (request.idleTimeout.count() > 0 && Clock::now() - lastActivity > request.idleTimeout) {
            idleTimedOut = true;
            break;
        }
    }
    CURLcode code = CURLE_OK;
    int pending = 0;
    if (CURLMsg* message = curl_multi_info_read(multi, &pending); message != nullptr) {
        code = message->data.result;
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (request.signal != nullptr) {
        request.signal->removeListener(listenerId);
    }
    curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headerList);

    if (aborted) {
        return std::unexpected(Error{"aborted", "request aborted"});
    }
    if (idleTimedOut || code == CURLE_OPERATION_TIMEDOUT) {
        return std::unexpected(Error{"timeout", "request timed out"});
    }
    if (code != CURLE_OK) {
        return std::unexpected(Error{"http_transport", curl_easy_strerror(code)});
    }
    response.status = static_cast<int>(status);
    return response;
}
