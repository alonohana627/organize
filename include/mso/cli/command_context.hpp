#pragma once
/// @file
/// Shared state of the `mso` command-line tool: global options, the lazily started Application,
/// output helpers and the process entry points. Every command stores a process exit code.
///
/// The CLI is a one-shot process: it opens libraries explicitly by path, never records them as
/// "open" in the workspace (the desktop app must not auto-reopen what a script touched), keeps no
/// in-memory index and runs the analysis supervisor only for `analyze`. A library that another
/// process (the desktop app) holds open is opened read-only: read commands work, write commands
/// fail with a clear message.
#include <CLI/CLI11.hpp>

#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "mso/app/application.hpp"

namespace mso::cli {

/// Process exit codes.
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;        ///< a runtime error (message on stderr)
inline constexpr int kExitUsage = 2;          ///< invalid command line (CLI11 parse errors included)
inline constexpr int kExitPartial = 3;        ///< completed, but results are incomplete (offline library, failed jobs)
inline constexpr int kExitInterrupted = 130;  ///< stopped by Ctrl+C (128 + SIGINT, the shell convention)

/// Whether a command needs to write the library.
enum class Access { Read, Write };

class CommandContext {
public:
    /// Output goes to `output`, diagnostics to `errors` (tests pass temporary files).
    explicit CommandContext(std::FILE* output = stdout, std::FILE* errors = stderr);
    ~CommandContext();
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    /// Global `--workspace` option (default: the per-user workspace).
    std::string workspace_directory;
    /// Global `--worker` option: the mso-worker executable (default: next to `mso`).
    std::string worker_executable;
    /// Global `--json` flag: machine-readable output where supported.
    bool json_output = false;

    /// Starts the application on first use. Libraries are opened explicitly by path, the root
    /// monitor stays off, and the analysis supervisor runs only when the first caller asks for it.
    Result<app::Application*> application(bool with_analysis = false);

    /// Opens (or returns the already open) library at a directory. With Access::Write, a library
    /// another process holds (opened read-only) is refused with DB_BUSY.
    Result<app::OpenLibraryInfo> open_library(const std::filesystem::path& directory, Access access = Access::Read);

    /// Prints one line of command output.
    template <class... Arguments>
    void print(std::format_string<Arguments...> format, Arguments&&... arguments) const {
        std::println(output_, format, std::forward<Arguments>(arguments)...);
    }
    /// Prints a transient progress line (only to a terminal, never with --json).
    template <class... Arguments>
    void progress(std::format_string<Arguments...> format, Arguments&&... arguments) const {
        if (!show_progress()) return;
        std::print(output_, "\r{}   ", std::format(format, std::forward<Arguments>(arguments)...));
        std::fflush(output_);
    }
    /// Ends a progress line started by progress().
    void end_progress() const;
    /// Prints a warning to the error stream.
    template <class... Arguments>
    void warn(std::format_string<Arguments...> format, Arguments&&... arguments) const {
        std::println(errors_, "warning: {}", std::format(format, std::forward<Arguments>(arguments)...));
    }

    /// Prints an error in a consistent format and returns kExitFailure.
    int report(const Error& error) const;
    /// Prints a usage error and returns kExitUsage.
    int usage(std::string_view message) const;

    std::FILE* output() const { return output_; }
    std::FILE* errors() const { return errors_; }

private:
    /// True when progress lines make sense: a terminal, and not --json.
    bool show_progress() const;

    std::FILE* output_;
    std::FILE* errors_;
    std::unique_ptr<app::Application> application_;
};

/// Each group registers its subcommands; the callback stores the exit code in `exit_code`.
void register_library_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_search_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_edit_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_analysis_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_media_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_bench_commands(CLI::App& root, CommandContext& context, int& exit_code);

/// Runs one command line in-process. `arguments` excludes the program name and must be UTF-8.
/// Never throws: parse errors exit kExitUsage, any escaping exception is reported and exits
/// kExitFailure. Used by main() and by the CLI tests.
int run(std::span<const std::string> arguments, std::FILE* output = stdout, std::FILE* errors = stderr);

/// Process entry point: converts the native command line to UTF-8, then run().
int run_main(int argc, char** argv);

/// Parses a BPM value ("128", "92.5") into milli-BPM: full consumption, finite, 1..999 BPM.
/// Locale-independent (std::from_chars).
Result<std::int64_t> parse_bpm_milli(std::string_view text);

}  // namespace mso::cli
