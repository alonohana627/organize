// Library, root, import, history, backup and export commands.
#include <stop_token>

#include "mso/cli/command_context.hpp"
#include "mso/cli/interrupt.hpp"

namespace mso::cli {

namespace {

/// One line of `info` describing a root and where it is bound on this computer.
void print_root(const CommandContext& context, const catalog::RootRecord& root,
                const std::optional<std::filesystem::path>& path) {
    context.print("  root {:>3}  {:<24} {:<10} {:<8} {:>7} samples  {}", root.id, root.name, to_string(root.mode),
                  to_string(root.availability), root.sample_count, path ? path_to_utf8(*path) : "(not bound here)");
}

/// Registers `create`.
void register_create(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("create", "Create a new library folder");
    /// Every option of `create`.
    struct Options {
        std::string directory;
        std::string name = "My Samples";
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "New (empty) library folder")->required();
    command->add_option("--name", options.name, "Library name");
    command->callback([&] {
        auto application = context.application();
        if (!application) return void(exit_code = context.report(application.error()));
        auto created = (*application)->create_library(path_from_utf8(options.directory), options.name);
        if (!created) return void(exit_code = context.report(created.error()));
        context.print("created library '{}' ({})", created->name, uuids::to_string(created->uuid));
    });
}

/// Registers `info`.
void register_info(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("info", "Show library identity, roots and counts");
    auto& directory = context.command_options<std::string>();
    command->add_option("library", directory, "Library folder")->required();
    command->callback([&] {
        auto library = context.open_library(path_from_utf8(directory));
        if (!library) return void(exit_code = context.report(library.error()));
        auto* application = *context.application();
        auto shown = application->with_library(library->uuid, [&](catalog::Library& opened) -> Result<void> {
            const auto identity = opened.identity();
            auto count = opened.sample_count();
            if (!count) return std::unexpected(count.error());
            auto roots = opened.list_roots();
            if (!roots) return std::unexpected(roots.error());
            context.print("library   {} ({})", identity.name, uuids::to_string(identity.uuid));
            context.print("schema    v{}  revision {}{}{}", identity.schema_version, identity.catalog_revision,
                          opened.read_only() ? "  (read-only)" : "",
                          library->in_use_elsewhere ? "  (in use by another process)" : "");
            context.print("samples   {}", *count);
            for (const auto& entry : *roots)
                print_root(context, entry, application->root_path(identity.uuid, entry));
            const auto& recovery = opened.recovery_report();
            if (recovery.interrupted_scans || recovery.partial_history_actions || recovery.interrupted_jobs)
                context.print("recovered {} interrupted scan(s), {} partial edit(s), {} interrupted job(s)",
                              recovery.interrupted_scans, recovery.partial_history_actions, recovery.interrupted_jobs);
            return {};
        });
        if (!shown) exit_code = context.report(shown.error());
    });
}

/// Registers `add-root`.
void register_add_root(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("add-root", "Reference a sample folder in place (files are never moved)");
    /// Every option of `add-root`.
    struct Options {
        std::string directory;
        std::string folder;
        std::string name;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("folder", options.folder, "Sample folder to reference")->required();
    command->add_option("--name", options.name, "Display name of the root");
    command->callback([&] {
        auto library = context.open_library(path_from_utf8(options.directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto added = (*context.application())->add_root(library->uuid, path_from_utf8(options.folder), options.name);
        if (!added) return void(exit_code = context.report(added.error()));
        context.print("added root {} '{}'", added->id, added->name);
    });
}

/// Registers `import`.
void register_import(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("import", "Scan roots: add new files, notice changes, moves and missing files");
    /// Every option of `import`.
    struct Options {
        std::string directory;
        std::optional<std::int64_t> root_id;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("--root", options.root_id, "Only this root id (default: all roots)");
    command->callback([&] {
        const auto& root_id = options.root_id;
        auto library = context.open_library(path_from_utf8(options.directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto* application = *context.application();
        auto roots =
            application->with_library(library->uuid, [](catalog::Library& opened) { return opened.list_roots(); });
        if (!roots) return void(exit_code = context.report(roots.error()));
        if (root_id && std::ranges::find(*roots, *root_id, &catalog::RootRecord::id) == roots->end())
            return void(exit_code = context.report(Error{
                            ErrorCode::NotFound, false, std::format("the library has no root {}", *root_id), {}, {}}));
        InterruptScope interrupt;
        bool incomplete = false;
        for (const auto& entry : *roots) {
            if (root_id && entry.id != *root_id) continue;
            context.print("scanning '{}'", entry.name);
            auto summary = application->import_now(
                library->uuid, entry.id, interrupt.token(), [&](const files::ImportProgress& progress) {
                    context.progress("  {} found, {} probed, {} committed", progress.files_discovered,
                                     progress.files_probed, progress.files_committed);
                });
            context.end_progress();
            if (!summary) {
                if (summary.error().code == ErrorCode::Cancelled) {
                    context.print("cancelled; everything committed so far is kept");
                    return void(exit_code = kExitInterrupted);
                }
                exit_code = context.report(summary.error());
                continue;
            }
            const auto& progress = summary->progress;
            incomplete = incomplete || !summary->finish.complete_coverage;
            context.print("  {} added, {} changed, {} renamed, {} marked missing, {} unreadable{}", progress.added,
                          progress.updated, progress.relinked, summary->finish.marked_missing, progress.probe_failures,
                          summary->finish.complete_coverage ? "" : "  (some folders could not be read)");
        }
        if (exit_code == kExitSuccess && incomplete) exit_code = kExitPartial;
    });
}

/// Registers `undo` and `redo`.
void register_history_steps(CLI::App& root, CommandContext& context, int& exit_code) {
    for (const auto* verb : {"undo", "redo"}) {
        const bool undo = std::string_view(verb) == "undo";
        auto* command = root.add_subcommand(
            verb, undo ? "Undo the last metadata edit (persists across restarts)" : "Redo the last undone edit");
        auto& directory = context.command_options<std::string>();
        command->add_option("library", directory, "Library folder")->required();
        command->callback([&, undo] {
            auto library = context.open_library(path_from_utf8(directory), Access::Write);
            if (!library) return void(exit_code = context.report(library.error()));
            auto* application = *context.application();
            auto receipt = undo ? application->undo(library->uuid) : application->redo(library->uuid);
            if (!receipt) return void(exit_code = context.report(receipt.error()));
            context.print("{} {} change(s), revision {}", undo ? "undid" : "redid", receipt->applied,
                          receipt->committed_revision);
            if (receipt->partial) {
                context.warn("only part of the action could be {}", undo ? "undone" : "redone");
                exit_code = kExitPartial;
            }
        });
    }
}

/// Registers `history`.
void register_history(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("history", "List recent edits");
    auto& directory = context.command_options<std::string>();
    command->add_option("library", directory, "Library folder")->required();
    command->callback([&] {
        auto library = context.open_library(path_from_utf8(directory));
        if (!library) return void(exit_code = context.report(library.error()));
        auto entries = (*context.application())->with_library(library->uuid, [](catalog::Library& opened) {
            return opened.history(50);
        });
        if (!entries) return void(exit_code = context.report(entries.error()));
        for (const auto& entry : *entries)
            context.print("{} {:>5}  {:<10} {}", entry.is_cursor ? '>' : ' ', entry.id, entry.state, entry.label);
    });
}

/// Registers `backup` and `check`.
void register_maintenance(CLI::App& root, CommandContext& context, int& exit_code) {
    {
        auto* command = root.add_subcommand("backup", "Write a verified backup into the library's backups folder");
        auto& directory = context.command_options<std::string>();
        command->add_option("library", directory, "Library folder")->required();
        command->callback([&] {
            // A backup writes and prunes the library's backups folder, so it needs the library's
            // lock like any other write (never while the desktop app owns the folder).
            auto library = context.open_library(path_from_utf8(directory), Access::Write);
            if (!library) return void(exit_code = context.report(library.error()));
            auto backup = (*context.application())->with_library(library->uuid, [](catalog::Library& opened) {
                return opened.create_backup("manual");
            });
            if (!backup) return void(exit_code = context.report(backup.error()));
            context.print("verified backup: {}", path_to_utf8(*backup));
        });
    }
    {
        auto* command = root.add_subcommand("check", "Run integrity, foreign-key and search-index checks");
        auto& directory = context.command_options<std::string>();
        command->add_option("library", directory, "Library folder")->required();
        command->callback([&] {
            auto library = context.open_library(path_from_utf8(directory));
            if (!library) return void(exit_code = context.report(library.error()));
            auto checked =
                (*context.application())->with_library(library->uuid, [&](catalog::Library& opened) -> Result<void> {
                    auto report = opened.integrity_report();
                    if (!report) return std::unexpected(report.error());
                    context.print("{}", *report);
                    auto review = opened.open_review_items(20);
                    if (!review) return std::unexpected(review.error());
                    if (!review->empty()) {
                        context.print("{} open review item(s):", review->size());
                        for (const auto& item : *review)
                            context.print("  {:<18} {}", item.kind, item.evidence_json);
                    }
                    return {};
                });
            if (!checked) exit_code = context.report(checked.error());
        });
    }
}

/// Registers `export`.
void register_export(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("export", "Export metadata to CSV or JSON (atomic; never touches audio)");
    /// Every option of `export`.
    struct Options {
        std::string directory;
        std::string destination;
        std::string format = "csv";
        std::string query;
        bool raw = false;
        bool absolute = false;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("file", options.destination, "Destination file")->required();
    command->add_option("--format", options.format, "csv or json")->check(CLI::IsMember({"csv", "json"}));
    command->add_option("--query", options.query, "Only samples matching this search");
    auto* raw_flag =
        command->add_flag("--raw", options.raw, "CSV: keep cells starting with = + - @ exactly (not spreadsheet-safe)");
    command->add_flag("--absolute-paths", options.absolute, "Include machine-local absolute paths");
    command->callback([&, raw_flag] {
        if (options.format == "json" && raw_flag->count() > 0)
            return void(exit_code = context.usage("--raw applies to CSV only"));
        auto library = context.open_library(path_from_utf8(options.directory));
        if (!library) return void(exit_code = context.report(library.error()));
        app::ExportOptions export_options;
        export_options.format = options.format == "json" ? app::ExportFormat::Json : app::ExportFormat::Csv;
        export_options.spreadsheet_safe = !options.raw;
        export_options.include_absolute_paths = options.absolute;
        export_options.query = options.query;
        InterruptScope interrupt;
        auto summary = (*context.application())
                           ->export_metadata(library->uuid, path_from_utf8(options.destination), export_options,
                                             interrupt.token());
        if (!summary) {
            if (summary.error().code == ErrorCode::Cancelled) return void(exit_code = kExitInterrupted);
            return void(exit_code = context.report(summary.error()));
        }
        context.print("exported {} sample(s) to {}", summary->rows, path_to_utf8(summary->file));
    });
}

}  // namespace

void register_library_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    register_create(root, context, exit_code);
    register_info(root, context, exit_code);
    register_add_root(root, context, exit_code);
    register_import(root, context, exit_code);
    register_history_steps(root, context, exit_code);
    register_history(root, context, exit_code);
    register_maintenance(root, context, exit_code);
    register_export(root, context, exit_code);
}

}  // namespace mso::cli
