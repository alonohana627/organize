// `mso`: command-line access to Music Sample Organizer libraries (scripting, tests, headless use).
#include "mso/cli/command_context.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace mso::cli {

Result<app::Application*> CommandContext::application(bool with_analysis) {
    if (!application_) {
        app::ApplicationOptions options;
        if (!workspace_directory.empty()) options.workspace_directory = path_from_utf8(workspace_directory);
        options.reopen_libraries = false;
        options.monitor_roots = false;
        options.run_analysis_supervisor = with_analysis;
        auto started = app::Application::start(options);
        if (!started) return std::unexpected(started.error());
        application_ = std::move(*started);
    }
    return application_.get();
}

Result<catalog::Library*> CommandContext::open_library(const std::filesystem::path& directory) {
    auto application = this->application();
    if (!application) return std::unexpected(application.error());
    std::error_code error;
    for (const auto& open : (*application)->libraries())
        if (std::filesystem::equivalent(open.directory, directory, error)) return (*application)->library(open.uuid);
    auto opened = (*application)->open_library(directory);
    if (!opened) return std::unexpected(opened.error());
    (*application)->refresh_root_availability();  // the CLI has no background root monitor
    return (*application)->library(opened->uuid);
}

int CommandContext::report(const Error& error) {
    std::println(stderr, "error: {}", error.message());
    return kExitFailure;
}

}  // namespace mso::cli

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // file names are UTF-8 (Hebrew, Japanese, ...)
#endif
    CLI::App application{"Music Sample Organizer command-line tool", "mso"};
    application.require_subcommand(1);
    application.set_version_flag("--version", "mso 0.1.0");
    mso::cli::CommandContext context;
    application.add_option("--workspace", context.workspace_directory,
                           "Workspace folder (machine-local library registry and root bindings)");
    application.add_flag("--json", context.json_output, "Machine-readable JSON output");

    int exit_code = mso::cli::kExitSuccess;
    mso::cli::register_library_commands(application, context, exit_code);
    mso::cli::register_search_commands(application, context, exit_code);
    mso::cli::register_edit_commands(application, context, exit_code);
    mso::cli::register_analysis_commands(application, context, exit_code);
    mso::cli::register_media_commands(application, context, exit_code);
    mso::cli::register_bench_commands(application, context, exit_code);

    argv = application.ensure_utf8(argv);
    try {
        application.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return application.exit(error);
    }
    return exit_code;
}
