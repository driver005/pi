export module pi.export_command;

import std;
export import pi.coding_services;
export import pi.types.command_line;
import pi.session.embedded_export_assets;
import pi.support.html_exporter;
import pi.support.path_resolver;
import pi.support.session_export_data;

/**
 * `pi export <session.jsonl> [output.html] [--theme dark|light]`: writes a session file as a self-contained HTML page (HtmlExporter),
 * by default `pi-session-<file name>.html` in the working directory. Like TypeScript's `--export`, the page has the conversation and
 * its tree but no system prompt or tool list, which only a live session knows. Returns the process exit code.
 */
export class ExportCommand {
public:
    ExportCommand(CodingServices& services, std::ostream& out, std::ostream& err)
        : m_services(services),
          m_out(out),
          m_err(err) {}

    int run(const CommandLine& line) {
        PlatformServices& platform = m_services.platform();
        const PathResolver paths(platform.files().homeDirectory());
        const std::string input = paths.resolveToCwd(line.arguments[0], line.options.cwd);
        if (!platform.files().exists(input)) {
            m_err << "pi: File not found: " << input << "\n";
            return 1;
        }
        auto session = m_services.sessions().open(input, std::nullopt, std::nullopt);
        if (!session) {
            m_err << "pi: " << session.error().message << "\n";
            return 1;
        }
        const EmbeddedExportAssets assets;
        const auto html = HtmlExporter(assets.assets(), platform.base64()).render(SessionExportData().build(**session, std::nullopt, std::nullopt), line.exportTheme);
        if (!html) {
            m_err << "pi: " << html.error().message << "\n";
            return 1;
        }
        const std::string name = "pi-session-" + std::filesystem::path(input).stem().string() + ".html";
        const std::string target = paths.resolveToCwd(line.arguments.size() > 1 ? line.arguments[1] : name, line.options.cwd);
        if (const auto written = platform.files().writeFile(target, *html); !written) {
            m_err << "pi: " << written.error().message << "\n";
            return 1;
        }
        m_out << "Exported to: " << target << "\n";
        return 0;
    }

private:
    CodingServices& m_services;
    std::ostream& m_out;
    std::ostream& m_err;
};
