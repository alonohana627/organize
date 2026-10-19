// edit, tag-rename/tag-move/tag-merge, collection commands.
#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

/// Parses "--bpm 128", "--bpm auto" or "--bpm unknown".
Result<catalog::BpmOverrideEdit> parse_bpm_edit(const std::string& text) {
    if (text == "auto") return catalog::BpmOverrideEdit{catalog::OverrideState::Automatic, std::nullopt};
    if (text == "unknown") return catalog::BpmOverrideEdit{catalog::OverrideState::Unknown, std::nullopt};
    try {
        const double bpm = std::stod(text);
        return catalog::BpmOverrideEdit{catalog::OverrideState::Value,
                                        static_cast<std::int64_t>(std::llround(bpm * 1000))};
    } catch (const std::exception&) {
        return fail(ErrorCode::InvalidArgument, "--bpm expects a number, 'auto' or 'unknown'");
    }
}

Result<catalog::KeyOverrideEdit> parse_key_edit(const std::string& text) {
    if (text == "auto") return catalog::KeyOverrideEdit{catalog::OverrideState::Automatic, std::nullopt};
    if (text == "unknown") return catalog::KeyOverrideEdit{catalog::OverrideState::Unknown, std::nullopt};
    auto key = parse_key(text);
    if (!key) return fail(ErrorCode::InvalidArgument, "--key expects a key like F#m, 'auto' or 'unknown'");
    return catalog::KeyOverrideEdit{catalog::OverrideState::Value, key};
}

void print_receipt(const catalog::MutationReceipt& receipt) {
    std::println("applied to {} sample(s), skipped {}, revision {}{}", receipt.applied, receipt.skipped,
                 receipt.committed_revision, receipt.replayed ? " (already applied earlier)" : "");
}

}  // namespace

void register_edit_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    {
        auto* command = root.add_subcommand("edit", "Edit metadata of samples (one undoable action)");
        struct Options {
            std::string directory;
            std::vector<std::int64_t> samples;
            std::optional<int> rating;
            std::optional<std::string> favorite, notes, name, category, bpm, key, operation;
            std::vector<std::string> tags_add, tags_remove;
            std::optional<std::int64_t> add_to, remove_from;
            bool remove = false, restore = false, clear_category = false;
        };
        auto options = std::make_shared<Options>();
        command->add_option("library", options->directory, "Library folder")->required();
        command->add_option("samples", options->samples, "Sample ids")->required();
        command->add_option("--rating", options->rating, "0-5")->check(CLI::Range(0, 5));
        command->add_option("--favorite", options->favorite, "yes or no")->check(CLI::IsMember({"yes", "no"}));
        command->add_option("--notes", options->notes, "Replace notes");
        command->add_option("--name", options->name, "Display name");
        command->add_option("--tag", options->tags_add, "Add tag path, e.g. Drums/Kick (repeatable)");
        command->add_option("--untag", options->tags_remove, "Remove tag path (repeatable)");
        command->add_option("--category", options->category, "Set category path");
        command->add_flag("--clear-category", options->clear_category, "Clear the category");
        command->add_option("--bpm", options->bpm, "BPM override: number, auto or unknown");
        command->add_option("--key", options->key, "Key override: e.g. F#m, auto or unknown");
        command->add_option("--add-to", options->add_to, "Add to manual collection id");
        command->add_option("--remove-from", options->remove_from, "Remove from manual collection id");
        command->add_flag("--remove", options->remove, "Remove from the library (soft, undoable; audio untouched)");
        command->add_flag("--restore", options->restore, "Restore removed samples");
        command->add_option("--operation", options->operation,
                            "Idempotency key (UUID): replays return the stored receipt");
        command->callback([&, options] {
            auto library = context.open_library(path_from_utf8(options->directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            catalog::EditCommand edit;
            if (options->operation) {
                auto uuid = uuids::uuid::from_string(*options->operation);
                if (!uuid)
                    return void(exit_code = CommandContext::report(
                                    Error{ErrorCode::InvalidArgument, false, "--operation must be a UUID", {}, {}}));
                edit.operation = *uuid;
            }
            edit.sample_ids = options->samples;
            edit.rating = options->rating;
            if (options->favorite) edit.favorite = *options->favorite == "yes";
            edit.notes = options->notes;
            edit.display_name = options->name;
            edit.tags_add = options->tags_add;
            edit.tags_remove = options->tags_remove;
            if (options->category) edit.category = std::optional<std::string>(*options->category);
            if (options->clear_category) edit.category = std::optional<std::string>();
            if (options->bpm) {
                auto bpm = parse_bpm_edit(*options->bpm);
                if (!bpm) return void(exit_code = CommandContext::report(bpm.error()));
                edit.bpm = *bpm;
            }
            if (options->key) {
                auto key = parse_key_edit(*options->key);
                if (!key) return void(exit_code = CommandContext::report(key.error()));
                edit.key = *key;
            }
            edit.add_to_collection = options->add_to;
            edit.remove_from_collection = options->remove_from;
            if (options->remove) edit.removed = true;
            if (options->restore) edit.removed = false;
            auto receipt = (*library)->apply_edit(edit);
            if (!receipt) return void(exit_code = CommandContext::report(receipt.error()));
            print_receipt(*receipt);
        });
    }
    {
        auto* command = root.add_subcommand("tag", "Rename, move or merge tags (undoable)");
        auto directory = std::make_shared<std::string>();
        auto tag = std::make_shared<std::int64_t>(0);
        auto rename = std::make_shared<std::string>();
        auto move_under = std::make_shared<std::optional<std::int64_t>>();
        auto to_top = std::make_shared<bool>(false);
        auto merge_into = std::make_shared<std::optional<std::int64_t>>();
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("tag", *tag, "Tag id (see `mso tags`)")->required();
        command->add_option("--rename", *rename, "New name");
        command->add_option("--move-under", *move_under, "New parent tag id");
        command->add_flag("--move-to-top", *to_top, "Make it a top-level tag");
        command->add_option("--merge-into", *merge_into, "Merge into this tag id");
        command->callback([&, directory, tag, rename, move_under, to_top, merge_into] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            catalog::TaxonomyCommand command_value{.target_id = *tag};
            if (!rename->empty()) {
                command_value.kind = catalog::TaxonomyCommand::Kind::RenameTag;
                command_value.new_name = *rename;
            } else if (*move_under || *to_top) {
                command_value.kind = catalog::TaxonomyCommand::Kind::MoveTag;
                command_value.new_parent_id = *move_under;
            } else if (*merge_into) {
                command_value.kind = catalog::TaxonomyCommand::Kind::MergeTag;
                command_value.other_id = *merge_into;
            } else {
                return void(exit_code = kExitUsage);
            }
            auto receipt = (*library)->apply_taxonomy(command_value);
            if (!receipt) return void(exit_code = CommandContext::report(receipt.error()));
            print_receipt(*receipt);
        });
    }
    {
        auto* command = root.add_subcommand("collection", "Create, rename or delete a collection (undoable)");
        auto directory = std::make_shared<std::string>();
        auto create = std::make_shared<std::string>();
        auto smart = std::make_shared<std::string>();
        auto target = std::make_shared<std::int64_t>(0);
        auto rename = std::make_shared<std::string>();
        auto remove = std::make_shared<bool>(false);
        command->add_option("library", *directory, "Library folder")->required();
        command->add_option("--create", *create, "Name of a new collection");
        command->add_option("--smart", *smart, "With --create: the query of a smart collection");
        command->add_option("--id", *target, "Existing collection id");
        command->add_option("--rename", *rename, "New name (with --id)");
        command->add_flag("--delete", *remove, "Delete (with --id; memberships are kept for undo)");
        command->callback([&, directory, create, smart, target, rename, remove] {
            auto library = context.open_library(path_from_utf8(*directory));
            if (!library) return void(exit_code = CommandContext::report(library.error()));
            catalog::TaxonomyCommand command_value{.target_id = *target};
            if (!create->empty()) {
                if (!smart->empty())
                    if (auto valid = search::SearchService().validate(*smart); !valid)
                        return void(exit_code = CommandContext::report(valid.error()));
                command_value.kind = catalog::TaxonomyCommand::Kind::CreateCollection;
                command_value.new_name = *create;
                if (!smart->empty()) command_value.smart_query = *smart;
            } else if (!rename->empty()) {
                command_value.kind = catalog::TaxonomyCommand::Kind::RenameCollection;
                command_value.new_name = *rename;
            } else if (*remove) {
                command_value.kind = catalog::TaxonomyCommand::Kind::DeleteCollection;
            } else {
                return void(exit_code = kExitUsage);
            }
            auto receipt = (*library)->apply_taxonomy(command_value);
            if (!receipt) return void(exit_code = CommandContext::report(receipt.error()));
            print_receipt(*receipt);
        });
    }
}

}  // namespace mso::cli
