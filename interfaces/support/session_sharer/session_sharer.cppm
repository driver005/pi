export module pi.support.session_sharer;

import std;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.platform.i_http_client;
export import pi.platform.i_id_generator;
export import pi.platform.i_process_runner;
export import pi.support.abort_signal;
export import pi.types.json;
export import pi.types.result;
export import pi.types.share_outcome;

/**
 * Shares a session. A signed-in Radius account gets the session uploaded as an artifact (`POST /v1/artifacts` on the default
 * gateway, the JSONL as `application/x-ndjson`, visible to the organization) and the artifact's canonical URL comes back; a failed
 * upload is an error, it does not fall back. Without a Radius account the session is rendered as HTML and shared as a private
 * gist through the GitHub CLI (`gh auth status`, then `gh gist create --public=false`); the link is the share viewer
 * (`PI_SHARE_VIEWER_URL`, default https://pi.dev/session/) with `#<gist id>`. Port of shareSession in
 * modes/interactive/session-share.ts without the terminal UI.
 */
export class SessionSharer {
public:
    SessionSharer(IHttpClient& http, IProcessRunner& processes, IFileSystem& files, const IEnvironment& environment, IIdGenerator& ids)
        : m_http(http),
          m_processes(processes),
          m_files(files),
          m_environment(environment),
          m_ids(ids) {}

    /** `renderHtml` produces the page only when the gist route needs it. */
    Result<ShareOutcome> share(const std::string& sessionJsonl, const std::function<Result<std::string>()>& renderHtml, const std::optional<std::string>& radiusToken,
                               const std::shared_ptr<AbortSignal>& signal) {
        if (radiusToken && !radiusToken->empty()) {
            return viaRadius(sessionJsonl, *radiusToken, signal);
        }
        return viaGist(renderHtml, signal);
    }

private:
    Result<ShareOutcome> viaRadius(const std::string& jsonl, const std::string& token, const std::shared_ptr<AbortSignal>& signal) {
        HttpRequest request;
        request.method = "POST";
        request.url = "https://radius.pi.dev/v1/artifacts?visibility=organization&title=Pi+session";
        request.headers = {{"authorization", "Bearer " + token}, {"content-type", "application/x-ndjson"}};
        request.body = jsonl;
        request.signal = signal;
        auto response = m_http.send(request);
        if (!response) {
            return std::unexpected(Error{"share_failed", "Failed to upload Radius artifact: " + response.error().message});
        }
        const Json body = Json::parse(response->body, nullptr, false);
        const bool ok = response->status >= 200 && response->status < 300;
        if (!ok || !body.is_object() || !body.value("artifact", Json()).is_object() || !body["artifact"].value("canonical_url", Json()).is_string()) {
            const std::string reason = body.is_object() && body.value("error", Json()).is_string() && !body["error"].get<std::string>().empty() ? body["error"].get<std::string>() : std::to_string(response->status);
            return std::unexpected(Error{"share_failed", "Failed to upload Radius artifact: " + reason});
        }
        ShareOutcome outcome;
        outcome.via = "radius";
        outcome.url = body["artifact"]["canonical_url"].get<std::string>();
        return outcome;
    }

    Result<ShareOutcome> viaGist(const std::function<Result<std::string>()>& renderHtml, const std::shared_ptr<AbortSignal>& signal) {
        ProcessRequest auth;
        auth.command = "gh";
        auth.args = {"auth", "status"};
        auth.signal = signal;
        const auto status = m_processes.run(auth);
        if (!status) {
            return std::unexpected(Error{"share_failed", "GitHub CLI (gh) is not installed. Install it from https://cli.github.com/"});
        }
        if (status->exitCode != 0) {
            return std::unexpected(Error{"share_failed", "GitHub CLI is not logged in. Run 'gh auth login' first."});
        }
        auto html = renderHtml();
        if (!html) {
            return std::unexpected(Error{"share_failed", "Failed to export session: " + html.error().message});
        }
        const std::string directory = m_environment.get("TMPDIR").value_or("/tmp") + "/pi-share-" + m_ids.next();
        if (auto created = m_files.createPrivateDirectories(directory); !created) {
            return std::unexpected(Error{"share_failed", "Failed to export session: " + created.error().message});
        }
        const std::string file = directory + "/session.html";
        auto written = m_files.writeFile(file, *html);
        Result<ShareOutcome> outcome = written ? createGist(file, signal) : Result<ShareOutcome>(std::unexpected(Error{"share_failed", "Failed to export session: " + written.error().message}));
        (void)m_files.removeTree(directory);
        return outcome;
    }

    Result<ShareOutcome> createGist(const std::string& file, const std::shared_ptr<AbortSignal>& signal) {
        ProcessRequest create;
        create.command = "gh";
        create.args = {"gist", "create", "--public=false", file};
        create.signal = signal;
        const auto result = m_processes.run(create);
        const std::string output = result ? trim(result->output) : std::string();
        if (!result || result->exitCode != 0) {
            return std::unexpected(Error{"share_failed", "Failed to create gist: " + (output.empty() ? (result ? "Unknown error" : result.error().message) : output)});
        }
        // The merged output ends with the gist URL.
        const std::size_t lineStart = output.rfind('\n');
        const std::string gistUrl = trim(lineStart == std::string::npos ? output : output.substr(lineStart + 1));
        const std::string gistId = gistUrl.substr(gistUrl.rfind('/') == std::string::npos ? 0 : gistUrl.rfind('/') + 1);
        if (gistId.empty()) {
            return std::unexpected(Error{"share_failed", "Failed to parse gist ID from gh output"});
        }
        const std::string viewer = m_environment.get("PI_SHARE_VIEWER_URL").value_or("");
        ShareOutcome outcome;
        outcome.via = "gist";
        outcome.url = (viewer.empty() ? "https://pi.dev/session/" : viewer) + "#" + gistId;
        outcome.gistUrl = gistUrl;
        return outcome;
    }

    std::string trim(const std::string& text) const {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    IHttpClient& m_http;
    IProcessRunner& m_processes;
    IFileSystem& m_files;
    const IEnvironment& m_environment;
    IIdGenerator& m_ids;
};
