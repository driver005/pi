import std;
import pi.base.posix_callback_server;
import pi.base.posix_signal_waiter;
import pi.base.stdio_byte_input;
import pi.base.stdio_byte_output;
import pi.base.system_environment;
import pi.coding_application;
import pi.coding_runtime_factory;
import pi.coding_services;
import pi.durable_serve;
import pi.auth_command;
import pi.mcp_command;
import pi.package_command;
import pi.serve_application;
import pi.support.command_line_parser;

int main(int argc, char** argv) {
    SystemEnvironment environment;
    const CommandLineParser parser(environment);
    std::error_code ignored;
    const std::vector<std::string> args(argv + 1, argv + argc);
    const auto line = parser.parse(args, std::filesystem::current_path(ignored).string());
    if (!line) {
        std::cerr << "pi: " << line.error().message << "\n\n" << parser.usage();
        return 2;
    }
    if (line->help) {
        std::cout << parser.usage();
        return 0;
    }
    if (line->command == "serve") {
        PosixSignalWaiter signals;
        signals.block();
        const CodingApplicationOptions& options = line->options;
        CodingServices services(options.agentDir,
                                options.catalogDir.empty() ? options.agentDir + "/catalog" : options.catalogDir,
                                options.faux);
        CodingRuntimeFactory runtimes(services, options.startup);
        PlatformServices& platform = services.platform();
        ServeDependencies dependencies;
        dependencies.files = &platform.files();
        dependencies.clock = &platform.clock();
        dependencies.ids = &platform.ids();
        dependencies.crypto = &platform.crypto();
        dependencies.logger = &platform.logger();
        dependencies.sessions = &services.sessions();
        dependencies.runtimes = &runtimes;
        dependencies.models = &services.models().models();
        std::unique_ptr<DurableServe> durable;
        std::shared_ptr<ISessionOpener> opener;
        if (!line->sessionTree) {
            durable = std::make_unique<DurableServe>(services, options.startup, options.agentDir);
            opener = durable->opener();
        }
        ServeApplication server(*line, dependencies, opener);
        if (const auto started = server.start(); !started) {
            std::cerr << "pi: " << started.error().message << "\n";
            return 1;
        }
        std::cerr << "pi: serving " << server.serverId() << " on " << server.socketPath() << "\n";
        signals.wait();
        server.stop();
        return 0;
    }
    if (line->command == "mcp") {
        const CodingApplicationOptions& options = line->options;
        CodingServices services(options.agentDir, options.catalogDir.empty() ? options.agentDir + "/catalog" : options.catalogDir, options.faux);
        McpCommand command(services, services.platform().http(), []() { return std::unique_ptr<ICallbackServer>(std::make_unique<PosixCallbackServer>()); }, std::cout, std::cerr);
        return command.run(*line);
    }
    if (line->command == "auth") {
        const CodingApplicationOptions& options = line->options;
        CodingServices services(options.agentDir, options.catalogDir.empty() ? options.agentDir + "/catalog" : options.catalogDir, options.faux);
        AuthCommand command(services, services.platform().http(), services.platform().sleeper(), []() { return std::unique_ptr<ICallbackServer>(std::make_unique<PosixCallbackServer>()); }, std::cin, std::cout, std::cerr);
        return command.run(*line);
    }
    if (line->command == "install" || line->command == "remove" || line->command == "update" || line->command == "list") {
        const CodingApplicationOptions& options = line->options;
        CodingServices services(options.agentDir, options.catalogDir.empty() ? options.agentDir + "/catalog" : options.catalogDir, options.faux);
        PackageCommand command(services, std::cout, std::cerr);
        return command.run(*line);
    }
    CodingApplication application(line->options);
    if (const auto opened = application.open(); !opened) {
        std::cerr << "pi: " << opened.error().message << "\n";
        return 1;
    }
    for (const auto& diagnostic : application.diagnostics()) {
        std::cerr << "pi: " << diagnostic.type << ": " << diagnostic.message << "\n";
    }
    StdioByteInput input;
    StdioByteOutput output;
    return application.runRpc(input, output);
}
