// Library, root, import, history, backup and export commands.
#include <atomic>
#include <csignal>
#include <stop_token>

#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

/// Ctrl+C requests cooperative cancellation of the running import.
std::stop_source g_interrupt;
void on_interrupt(int) {
    g_interrupt.request_stop();
}

void print_root(const catalog::RootRecord& root, const std::optional<std::filesystem::path>& path) {
    std::println("  root {:>3}  {:<24} {:<10} {:<8} {:>7} samples  {}", root.id, root.name, to_string(root.mode),
                 to_string(root.availability), root.sample_count, path ? path_to_utf8(*path) : "(not bound here)");
}

}  // namespace

void register_library_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    // ---------------------------------------------------------------- create
    {
        auto* command = root.add_subcommand("create", "Create a new library folder");
        auto directory = std::make_shared<std::string>();
        auto name = std::make_shared<std::string>("My Samples");
        command->add_option("library", *directory, "New (empty) library folder")->required();
        command->add_option("--name", *name, "Library name");
        command->callback([&, directory, name] {
            auto application = context.application();
            if (!application) return void(exit_code = CommandContext::report(application.error()));
            auto created = (*application)->create_library(path_from_utf8(*directory), *name);
            if (!created) return void(exit_code = CommandContext::report(created.error()));
            std::println("created library '{}' ({})", created->name, uuids::to_string(created->uuid));
        });
    }
    // ---------------------------------------------------------------- info
    {
        auto* command = root.add_subcommand("info", "Show library identity, roots and counts");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            const auto identity = (*library)->identity();
            auto count = (*library)->sample_count();
            std::println("library   {} ({})", identity.name, uuids::to_string(identity.uuid));
            std::println("schema    v{}  revision {}{}", identity.schema_version, identity.catalog_revision,
                         (*library)->read_only() ? "  (read-only)" : "");
            std::println("samples   {}", count.value_or(0));
            auto roots = (*library)->list_roots();
            if (!roots) return void(exit_code = CommandContext::report(roots.error()));
            auto* application = *context.application();
            for (const auto& entry : *roots)
                print_root(entry, application->root_path(identity.uuid, entry));
            const auto& recovery = (*library)->recovery_report();
            if (recovery.interrupted_scans || recovery.partial_history_actions || recovery.interrupted_jobs)
                std::println("recovered {} interrupted scan(s), {} partial edit(s), {} interrupted job(s)",
                             recovery.interrupted_scans, recovery.partial_history_actions, recovery.interrupted_jobs);
        });
    }
    // ---------------------------------------------------------------- add-root
    {
        auto* command = root.add_subcommand("add-root", "Reference a sample folder in place (files are never moved)");
        auto directory = std::make_shared<std::string>();
        auto folder = std::make_shared<std::string>();
        auto name = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("folder", *folder, "Sample folder to reference")->required();
        command->add_option("--name", *name, "Display name of the root");
        command->callback([&, directory, folder, name] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto added = (*context.application())->add_root((*library)->uuid(), path_from_utf8(*folder), *name);
            if (!added) return void(exit_code = CommandContext::report(added.error()));
            std::println("added root {} '{}'", added->id, added->name);
        });
    }
    // ---------------------------------------------------------------- import
    {
        auto* command =
            root.add_subcommand("import", "Scan roots: add new files, notice changes, moves and missing files");
        auto directory = std::make_shared<std::string>();
        auto root_id = std::make_shared<std::int64_t>(0);
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("--root", *root_id, "Only this root (default: all roots)");
        command->callback([&, directory, root_id] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto roots = (*library)->list_roots();
            if (!roots) return void(exit_code = CommandContext::report(roots.error()));
            std::signal(SIGINT, on_interrupt);
            for (const auto& entry : *roots) {
                if (*root_id && entry.id != *root_id) continue;
                std::println("scanning '{}'", entry.name);
                auto summary = (*context.application())
                                   ->import_now((*library)->uuid(), entry.id, g_interrupt.get_token(),
                                                [](const files::ImportProgress& progress) {
                                                    std::print("\r  {} found, {} probed, {} committed   ",
                                                               progress.files_discovered, progress.files_probed,
                                                               progress.files_committed);
                                                    std::fflush(stdout);
                                                });
                std::println("");
                if (!summary) {
                    exit_code = CommandContext::report(summary.error());
                    if (summary.error().code == ErrorCode::Cancelled) return;
                    continue;
                }
                const auto& progress = summary->progress;
                std::println("  {} added, {} changed, {} renamed, {} marked missing, {} unreadable{}", progress.added,
                             progress.updated, progress.relinked, summary->finish.marked_missing,
                             progress.probe_failures,
                             summary->finish.complete_coverage ? "" : "  (some folders could not be read)");
            }
        });
    }
    // ---------------------------------------------------------------- history
    for (const auto* verb : {"undo", "redo"}) {
        const bool undo = std::string_view(verb) == "undo";
        auto* command = root.add_subcommand(
            verb, undo ? "Undo the last metadata edit (persists across restarts)" : "Redo the last undone edit");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory, undo] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto receipt = undo ? (*library)->undo() : (*library)->redo();
            if (!receipt) return void(exit_code = CommandContext::report(receipt.error()));
            std::println("{} {} change(s), revision {}", undo ? "undid" : "redid", receipt->applied,
                         receipt->committed_revision);
        });
    }
    {
        auto* command = root.add_subcommand("history", "List recent edits");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto entries = (*library)->history(50);
            if (!entries) return void(exit_code = CommandContext::report(entries.error()));
            for (const auto& entry : *entries)
                std::println("{} {:>5}  {:<10} {}", entry.is_cursor ? '>' : ' ', entry.id, entry.state, entry.label);
        });
    }
    // ---------------------------------------------------------------- maintenance
    {
        auto* command = root.add_subcommand("backup", "Write a verified backup into the library's backups folder");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto backup = (*library)->create_backup("manual");
            if (!backup) return void(exit_code = CommandContext::report(backup.error()));
            std::println("verified backup: {}", path_to_utf8(*backup));
        });
    }
    {
        auto* command = root.add_subcommand("check", "Run integrity, foreign-key and search-index checks");
        auto directory = std::make_shared<std::string>();
        command->add_option("library", *directory, "Library folder")->required();
        command->callback([&, directory] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            auto report = (*library)->integrity_report();
            if (!report) return void(exit_code = CommandContext::report(report.error()));
            std::println("{}", *report);
            auto review = (*library)->open_review_items(20);
            if (review && !review->empty()) {
                std::println("{} open review item(s):", review->size());
                for (const auto& item : *review)
                    std::println("  {:<18} {}", item.kind, item.evidence_json);
            }
        });
    }
    // ---------------------------------------------------------------- export
    {
        auto* command = root.add_subcommand("export", "Export metadata to CSV or JSON (atomic; never touches audio)");
        auto directory = std::make_shared<std::string>();
        auto destination = std::make_shared<std::string>();
        auto format = std::make_shared<std::string>("csv");
        auto query = std::make_shared<std::string>();
        auto raw = std::make_shared<bool>(false);
        auto absolute = std::make_shared<bool>(false);
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("file", *destination, "Destination file")->required();
        command->add_option("--format", *format, "csv or json")->check(CLI::IsMember({"csv", "json"}));
        command->add_option("--query", *query, "Only samples matching this search");
        command->add_flag("--raw", *raw, "CSV: keep cells starting with = + - @ exactly (not spreadsheet-safe)");
        command->add_flag("--absolute-paths", *absolute, "Include machine-local absolute paths");
        command->callback([&, directory, destination, format, query, raw, absolute] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            app::ExportOptions options;
            options.format = *format == "json" ? app::ExportFormat::Json : app::ExportFormat::Csv;
            options.spreadsheet_safe = !*raw;
            options.include_absolute_paths = *absolute;
            options.query = *query;
            auto summary =
                (*context.application())->export_metadata((*library)->uuid(), path_from_utf8(*destination), options);
            if (!summary) return void(exit_code = CommandContext::report(summary.error()));
            std::println("exported {} sample(s) to {}", summary->rows, path_to_utf8(summary->file));
        });
    }
}

}  // namespace mso::cli
