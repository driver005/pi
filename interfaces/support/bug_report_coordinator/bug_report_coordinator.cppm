module;

#include <cstdint>

export module pi.support.bug_report_coordinator;

import std;
export import pi.platform.i_clock;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.session.i_session_manager;
export import pi.support.bug_report_builder;
export import pi.support.bug_report_summarizer;
export import pi.support.bug_report_uploader;
export import pi.support.crash_log;
export import pi.support.radius_gateway;
export import pi.support.session_branch_serializer;
export import pi.support.zip_writer;
export import pi.types.bug_report_input;
export import pi.types.bug_report_outcome;
export import pi.types.bug_report_request;
export import pi.types.summarization_options;

/**
 * Files a bug report: optionally has the session model write a summary, builds the bundle, then uploads it to the Radius
 * gateway or writes a zip archive, records the report in the session as a `pi.bug-report` entry and clears the crash log once
 * its crashes were reported. Uploads are refused while PI_OFFLINE is set. Port of the delivery half of
 * modes/interactive/bug-report.ts, without its consent dialogs (the caller decides what to include).
 */
export class BugReportCoordinator {
public:
    BugReportCoordinator(const BugReportBuilder& builder, const BugReportSummarizer& summarizer, const SessionBranchSerializer& serializer, const BugReportUploader& uploader, const CrashLog& crashes, IFileSystem& files, const IClock& clock, const IEnvironment& environment)
        : m_builder(builder),
          m_summarizer(summarizer),
          m_serializer(serializer),
          m_uploader(uploader),
          m_crashes(crashes),
          m_files(files),
          m_clock(clock),
          m_environment(environment) {}

    /** `radiusToken` yields the signed-in Radius account's token, or nothing for an anonymous upload. */
    Result<BugReportOutcome> report(const BugReportRequest& request, BugReportInput input, const std::vector<AgentMessage>& messages, const SummarizationOptions& summaryOptions, ISessionManager& session, const std::function<std::optional<std::string>()>& radiusToken, const std::string& agentDir, const std::string& cwd, const std::string& gatewayUrl) const {
        if (request.delivery != "upload" && request.delivery != "zip") {
            return std::unexpected(Error{"invalid_argument", "Bug report delivery must be \"upload\" or \"zip\""});
        }
        if (request.delivery == "upload" && m_environment.get("PI_OFFLINE")) {
            return std::unexpected(Error{"offline", "Uploading bug reports requires online mode. Use the zip delivery instead."});
        }
        BugReportBundle bundle;
        if (request.includeSummary) {
            auto summary = m_summarizer.summarize(messages, request.hint, summaryOptions);
            if (!summary) {
                return std::unexpected(Error{summary.error().code, "Failed to write bug report summary: " + summary.error().message});
            }
            bundle.summary = *summary;
        }
        input.hint = request.hint;
        input.includeSession = request.includeSession;
        input.includeSummary = bundle.summary.has_value();
        bundle.metadata = m_builder.metadata(input);
        const std::string crashPath = m_crashes.path(agentDir);
        bundle.diagnostics = m_builder.diagnostics(input.sessionId, session.entries(), m_crashes.read(crashPath));
        if (request.includeSession) {
            bundle.sessionJsonl = m_serializer.serialize(input.sessionId, cwd, session.branchPath());
        }
        const std::string id = bundle.metadata["id"].get<std::string>();
        BugReportOutcome outcome;
        outcome.id = id;
        outcome.delivery = request.delivery;
        if (request.delivery == "upload") {
            auto uploaded = m_uploader.upload(m_builder.files(bundle), gatewayUrl, radiusToken ? radiusToken() : std::nullopt);
            if (!uploaded) {
                return std::unexpected(uploaded.error());
            }
            outcome.id = *uploaded;
        } else {
            const std::string path = archivePath(request.outputPath, cwd, id);
            std::vector<ZipEntry> entries;
            for (const BugReportFile& file : m_builder.files(bundle)) {
                entries.push_back(ZipEntry{file.name, file.data});
            }
            if (auto written = m_files.writeFile(path, m_zip.create(entries, m_clock.nowMs())); !written) {
                return std::unexpected(Error{written.error().code, "Failed to write bug report: " + written.error().message});
            }
            outcome.path = path;
        }
        record(session, bundle, outcome);
        if (!bundle.diagnostics["crashes"].empty()) {
            m_crashes.clear(crashPath);
        }
        return outcome;
    }

private:
    /** An existing directory (or a path ending in `/`) receives the default file name; anything else is the file itself. */
    std::string archivePath(const std::optional<std::string>& output, const std::string& cwd, const std::string& id) const {
        const std::string name = m_builder.archiveFileName(id);
        if (!output || output->empty()) {
            return cwd + "/" + name;
        }
        const auto stat = m_files.stat(*output);
        if (output->ends_with("/") || (stat && stat->isDirectory)) {
            return (output->ends_with("/") ? *output : *output + "/") + name;
        }
        return *output;
    }

    void record(ISessionManager& session, const BugReportBundle& bundle, const BugReportOutcome& outcome) const {
        Json data = Json::object({{"id", bundle.metadata["id"]}, {"createdAt", bundle.metadata["createdAt"]}, {"hint", bundle.metadata["hint"]}, {"sessionIncluded", bundle.metadata["session"]["included"]}, {"summaryIncluded", bundle.metadata["session"]["summaryIncluded"]}, {"delivery", outcome.delivery}});
        if (outcome.path) {
            data["path"] = *outcome.path;
        }
        session.appendCustomEntry(m_builder.customEntryType(), data);
    }

    const BugReportBuilder& m_builder;
    const BugReportSummarizer& m_summarizer;
    const SessionBranchSerializer& m_serializer;
    const BugReportUploader& m_uploader;
    const CrashLog& m_crashes;
    IFileSystem& m_files;
    const IClock& m_clock;
    const IEnvironment& m_environment;
    ZipWriter m_zip;
};
