// The `mso` command line: CommandContext, the exception barrier and exit-code mapping, and the
// process entry point (main.cpp only calls run_main()).
#include <sstream>

#include "mso/cli/command_context.hpp"
#include "mso/generated/dependencies.hpp"

#ifdef _WIN32
#include <io.h>  // _isatty, _fileno
#else
#include <unistd.h>  // isatty, fileno
#endif

namespace mso::cli {

CommandContext::CommandContext(std::FILE* output, std::FILE* errors) : output_(output), errors_(errors) {}

CommandContext::~CommandContext() = default;

Result<app::Application*> CommandContext::application(bool with_analysis) {
    if (!application_) {
        app::ApplicationOptions options;
        if (!workspace_directory.empty()) options.workspace_directory = path_from_utf8(workspace_directory);
        if (!worker_executable.empty()) options.worker_executable = path_from_utf8(worker_executable);
        options.reopen_libraries = false;
        options.monitor_roots = false;
        options.run_analysis_supervisor = with_analysis;
        options.in_memory_index = false;         // one-shot process: SQLite answers directly
        options.persist_open_libraries = false;  // never make the desktop app reopen what a script touched
        auto started = app::Application::start(options);
        if (!started) return std::unexpected(started.error());
        application_ = std::move(*started);
    }
    return application_.get();
}

Result<app::OpenLibraryInfo> CommandContext::open_library(const std::filesystem::path& directory, Access access) {
    auto application = this->application();
    if (!application) return std::unexpected(application.error());
    std::optional<app::OpenLibraryInfo> info;
    for (const auto& open : (*application)->libraries()) {
        std::error_code error;  // an unreachable path simply is not the same library
        if (std::filesystem::equivalent(open.directory, directory, error)) {
            info = open;
            break;
        }
    }
    if (!info) {
        auto opened = (*application)->open_library(directory);
        if (!opened) return std::unexpected(opened.error());
        info = *opened;
        if (!info->read_only) (*application)->refresh_root_availability();  // no background monitor here
    }
    if (access == Access::Write && info->read_only) {
        if (info->in_use_elsewhere)
            return fail(ErrorCode::DbBusy,
                        "the library is open in another Music Sample Organizer process; close it there to make changes",
                        true);
        return fail(ErrorCode::AccessDenied, "the library can only be opened read-only here");
    }
    return *info;
}

bool CommandContext::show_progress() const {
    if (json_output) return false;
#ifdef _WIN32
    return _isatty(_fileno(output_)) != 0;
#else
    return isatty(fileno(output_)) != 0;
#endif
}

void CommandContext::end_progress() const {
    if (!show_progress()) return;
    std::println(output_, "");
}

int CommandContext::report(const Error& error) const {
    std::println(errors_, "error: {}", error.message());
    return kExitFailure;
}

int CommandContext::usage(std::string_view message) const {
    std::println(errors_, "usage error: {}", message);
    return kExitUsage;
}

int run(std::span<const std::string> arguments, std::FILE* output, std::FILE* errors) {
    // Exception barrier: nothing escapes to std::terminate, whatever a command or CLI11 throws.
    try {
        CLI::App application{"Music Sample Organizer command-line tool", "mso"};
        application.require_subcommand(1);
        application.set_version_flag("--version", std::format("mso {}", mso::generated::kProjectVersion));
        CommandContext context(output, errors);
        application.add_option("--workspace", context.workspace_directory,
                               "Workspace folder (machine-local library registry and root bindings)");
        application.add_option("--worker", context.worker_executable,
                               "mso-worker executable for analyze (default: next to mso)");
        application.add_flag("--json", context.json_output, "Machine-readable JSON output");

        int exit_code = kExitSuccess;
        register_library_commands(application, context, exit_code);
        register_search_commands(application, context, exit_code);
        register_edit_commands(application, context, exit_code);
        register_analysis_commands(application, context, exit_code);
        register_media_commands(application, context, exit_code);
        register_bench_commands(application, context, exit_code);

        std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());  // CLI11 consumes from the back
        try {
            application.parse(reversed);
        } catch (const CLI::ParseError& error) {
            std::ostringstream normal;
            std::ostringstream failure;
            const int code = application.exit(error, normal, failure);
            std::print(output, "{}", normal.str());
            std::print(errors, "{}", failure.str());
            return code == 0 ? kExitSuccess : kExitUsage;  // --help and --version are not errors
        }
        return exit_code;
    } catch (const std::exception& exception) {
        std::println(errors, "error: internal failure: {}", exception.what());
        return kExitFailure;
    }
}

int run_main(int argc, char** argv) {
    try {
        CLI::App converter;  // owns the UTF-8 copies of the arguments on Windows
        char** utf8_arguments = converter.ensure_utf8(argv);
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(utf8_arguments[index]);
        return run(arguments);
    } catch (const std::exception& exception) {
        std::println(stderr, "error: internal failure: {}", exception.what());
        return kExitFailure;
    }
}

}  // namespace mso::cli
