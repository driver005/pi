export module pi.support.bug_report_uploader;

import std;
export import pi.platform.i_http_client;
export import pi.platform.i_id_generator;
export import pi.types.bug_report_file;
export import pi.types.json;
export import pi.types.result;

/**
 * Uploads a bug report as multipart form data to `<gateway>/v1/bug-reports`, anonymously or as the signed-in Radius account.
 * The answer is `{ok: true, bug_report: {id}}`; anything else is a failure carrying the gateway's description. Port of
 * core/bug-report-upload.ts.
 */
export class BugReportUploader {
public:
    BugReportUploader(IHttpClient& http, IIdGenerator& ids)
        : m_http(http),
          m_ids(ids) {}

    /** The report id the gateway assigned. */
    Result<std::string> upload(const std::vector<BugReportFile>& files, const std::string& gatewayUrl, const std::optional<std::string>& token, const std::shared_ptr<AbortSignal>& signal = nullptr) const {
        const std::string boundary = "----pi-bug-report-" + m_ids.next();
        HttpRequest request;
        request.method = "POST";
        request.url = gatewayUrl + "/v1/bug-reports";
        request.headers = {{"Content-Type", "multipart/form-data; boundary=" + boundary}};
        if (token) {
            request.headers.emplace_back("Authorization", "Bearer " + *token);
        }
        request.body = multipart(files, boundary);
        request.signal = signal;
        request.timeout = std::chrono::minutes(2);
        const auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(Error{response.error().code, "Bug report upload failed: " + response.error().message});
        }
        const Json json = Json::parse(response->body, nullptr, false);
        if (response->status >= 200 && response->status < 300 && json.is_object() && json.value("ok", false) && json.contains("bug_report") && json["bug_report"].is_object() && json["bug_report"].contains("id") && json["bug_report"]["id"].is_string()) {
            return json["bug_report"]["id"].get<std::string>();
        }
        std::string detail;
        if (json.is_object() && !json.value("ok", false)) {
            detail = json.value("description", std::string());
            if (detail.empty()) {
                detail = json.value("error", std::string());
            }
        }
        if (detail.empty()) {
            detail = std::to_string(response->status);
        }
        return std::unexpected(Error{"upload_failed", "Bug report upload failed: " + detail});
    }

private:
    std::string multipart(const std::vector<BugReportFile>& files, const std::string& boundary) const {
        std::string body;
        for (const BugReportFile& file : files) {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + file.name + "\"; filename=\"" + file.name + "\"\r\nContent-Type: " + file.contentType + "\r\n\r\n" + file.data + "\r\n";
        }
        return body + "--" + boundary + "--\r\n";
    }

    IHttpClient& m_http;
    IIdGenerator& m_ids;
};
