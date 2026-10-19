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
#include <string_view>
#include <vector>

#include "mso/app/application.hpp"
#include "mso/cli/value_parsing.hpp"

namespace mso::cli {

/// Process exit codes.
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;        ///< a runtime error (message on stderr)
inline constexpr int kExitUsage = 2;          ///< invalid command line (CLI11 parse errors included)
inline constexpr int kExitPartial = 3;        ///< completed, but results are incomplete (offline library, failed jobs)
inline constexpr int kExitInterrupted = 130;  ///< stopped by Ctrl+C (128 + SIGINT, the shell convention)

/// Whether a command needs to write the library.
enum class Access { Read, Write };

/// Per-run state of the command line: global options, output streams, the Application started on
/// first use, and the storage every subcommand's options are parsed into.
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
    [[nodiscard]] Result<app::Application*> application(bool with_analysis = false);

    /// Opens (or returns the already open) library at a directory. With Access::Write, a library
    /// another process holds (opened read-only) is refused with DB_BUSY.
    [[nodiscard]] Result<app::OpenLibraryInfo> open_library(const std::filesystem::path& directory,
                                                            Access access = Access::Read);

    /// Storage for one subcommand's option values, owned by this context. CLI11 writes parsed
    /// values through references into it during parse(), long after the register_* function that
    /// bound them has returned, so binding targets must never be locals or objects whose last
    /// owner is a callback capture (`mso collection --delete` once wrote into freed memory that
    /// way). Every register_* function binds to members of one such object.
    template <class Options>
    Options& command_options() {
        auto stored = std::make_shared<Options>();
        Options& options = *stored;
        option_storage_.push_back(std::move(stored));
        return options;
    }

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
    [[nodiscard]] int report(const Error& error) const;
    /// Prints a usage error and returns kExitUsage.
    [[nodiscard]] int usage(std::string_view message) const;

    std::FILE* output() const { return output_; }
    std::FILE* errors() const { return errors_; }

private:
    /// True when progress lines make sense: a terminal, and not --json.
    bool show_progress() const;

    std::FILE* output_;
    std::FILE* errors_;
    /// Option values of every registered subcommand (see command_options()). Declared before
    /// application_ so the Application, which may still be running work, is destroyed first.
    std::vector<std::shared_ptr<void>> option_storage_;
    std::unique_ptr<app::Application> application_;
};

// Each group registers its subcommands on `root`; a subcommand's callback stores its exit code in
// `exit_code`. Option values live in `context` (CommandContext::command_options), so `root`,
// `context` and `exit_code` must outlive parsing.

/// Registers create, info, add-root, import, undo, redo, history, backup, check and export.
void register_library_commands(CLI::App& root, CommandContext& context, int& exit_code);
/// Registers search, show, tags and collections.
void register_search_commands(CLI::App& root, CommandContext& context, int& exit_code);
/// Registers edit, tag and collection.
void register_edit_commands(CLI::App& root, CommandContext& context, int& exit_code);
/// Registers analyze.
void register_analysis_commands(CLI::App& root, CommandContext& context, int& exit_code);
/// Registers probe, play and capabilities.
void register_media_commands(CLI::App& root, CommandContext& context, int& exit_code);
/// Registers bench.
void register_bench_commands(CLI::App& root, CommandContext& context, int& exit_code);

/// Runs one command line in-process. `arguments` excludes the program name and must be UTF-8.
/// Never throws: parse errors exit kExitUsage, any escaping exception is reported and exits
/// kExitFailure. Used by main() and by the CLI tests.
[[nodiscard]] int run(std::span<const std::string> arguments, std::FILE* output = stdout, std::FILE* errors = stderr);

/// Process entry point: converts the native command line to UTF-8, then run().
[[nodiscard]] int run_main(int argc, char** argv);

}  // namespace mso::cli
