#pragma once
/// @file
/// Shared state of the `mso` command-line tool: global options, the lazily started Application,
/// and output helpers. Every command returns a process exit code.
#include <CLI/CLI11.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>

#include "mso/app/application.hpp"

namespace mso::cli {

/// Process exit codes.
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;

class CommandContext {
public:
    /// Global `--workspace` option (default: the per-user workspace).
    std::string workspace_directory;
    /// Global `--json` flag: machine-readable output where supported.
    bool json_output = false;

    /// Starts the application on first use. Libraries are opened explicitly by path, the root
    /// monitor stays off, and the analysis supervisor runs only when a command needs it.
    Result<app::Application*> application(bool with_analysis = false);

    /// Opens (or returns the already open) library at a directory.
    Result<catalog::Library*> open_library(const std::filesystem::path& directory);

    /// Prints an error in a consistent format and returns kExitFailure.
    static int report(const Error& error);

private:
    std::unique_ptr<app::Application> application_;
};

/// Each group registers its subcommands; the callback stores the exit code in `exit_code`.
void register_library_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_search_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_edit_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_analysis_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_media_commands(CLI::App& root, CommandContext& context, int& exit_code);
void register_bench_commands(CLI::App& root, CommandContext& context, int& exit_code);

}  // namespace mso::cli
