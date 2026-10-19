// edit, tag (rename/move/merge) and collection commands.
#include "mso/cli/command_context.hpp"

namespace mso::cli {

namespace {

/// Parses "--bpm 128", "--bpm auto" or "--bpm unknown".
Result<catalog::BpmOverrideEdit> parse_bpm_edit(const std::string& text) {
    if (text == "auto") return catalog::BpmOverrideEdit{catalog::OverrideState::Automatic, std::nullopt};
    if (text == "unknown") return catalog::BpmOverrideEdit{catalog::OverrideState::Unknown, std::nullopt};
    auto milli_bpm = parse_bpm_milli(text);
    if (!milli_bpm)
        return fail(ErrorCode::InvalidArgument,
                    std::format("--bpm expects a number between 1 and 999, 'auto' or 'unknown' (got '{}')", text));
    return catalog::BpmOverrideEdit{catalog::OverrideState::Value, *milli_bpm};
}

/// Parses "--key F#m", "--key auto" or "--key unknown".
Result<catalog::KeyOverrideEdit> parse_key_edit(const std::string& text) {
    if (text == "auto") return catalog::KeyOverrideEdit{catalog::OverrideState::Automatic, std::nullopt};
    if (text == "unknown") return catalog::KeyOverrideEdit{catalog::OverrideState::Unknown, std::nullopt};
    auto key = parse_key(text);
    if (!key) return fail(ErrorCode::InvalidArgument, "--key expects a key like F#m, 'auto' or 'unknown'");
    return catalog::KeyOverrideEdit{catalog::OverrideState::Value, key};
}

/// Prints a receipt; returns kExitPartial when only part of the edit was applied.
int print_receipt(const CommandContext& context, const catalog::MutationReceipt& receipt) {
    context.print("applied to {} sample(s), skipped {}, revision {}{}", receipt.applied, receipt.skipped,
                  receipt.committed_revision, receipt.replayed ? " (already applied earlier)" : "");
    if (!receipt.partial) return kExitSuccess;
    context.warn("only part of the edit was applied; the remaining samples were not changed");
    return kExitPartial;
}

/// Registers `edit`.
void register_edit(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("edit", "Edit metadata of samples (one undoable action)");
    /// Every option of `edit`.
    struct Options {
        std::string directory;
        std::vector<std::int64_t> samples;
        std::optional<int> rating;
        std::optional<std::string> favorite, notes, name, category, bpm, key, operation;
        std::vector<std::string> tags_add, tags_remove;
        std::optional<std::int64_t> add_to, remove_from;
        bool remove = false, restore = false, clear_category = false;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("samples", options.samples, "Sample ids")->required();
    command->add_option("--rating", options.rating, "0-5")->check(CLI::Range(0, 5));
    command->add_option("--favorite", options.favorite, "yes or no")->check(CLI::IsMember({"yes", "no"}));
    command->add_option("--notes", options.notes, "Replace notes");
    command->add_option("--name", options.name, "Display name");
    command->add_option("--tag", options.tags_add, "Add tag path, e.g. Drums/Kick (repeatable)");
    command->add_option("--untag", options.tags_remove, "Remove tag path (repeatable)");
    auto* category = command->add_option("--category", options.category, "Set category path");
    auto* clear_category = command->add_flag("--clear-category", options.clear_category, "Clear the category");
    category->excludes(clear_category);
    command->add_option("--bpm", options.bpm, "BPM override: number, auto or unknown");
    command->add_option("--key", options.key, "Key override: e.g. F#m, auto or unknown");
    auto* add_to = command->add_option("--add-to", options.add_to, "Add to manual collection id");
    auto* remove_from = command->add_option("--remove-from", options.remove_from, "Remove from manual collection id");
    add_to->excludes(remove_from);
    auto* remove =
        command->add_flag("--remove", options.remove, "Remove from the library (soft, undoable; audio untouched)");
    auto* restore = command->add_flag("--restore", options.restore, "Restore removed samples");
    remove->excludes(restore);
    command->add_option("--operation", options.operation, "Idempotency key (UUID): replays return the stored receipt");
    command->callback([&] {
        catalog::EditCommand edit;
        if (options.operation) {
            auto uuid = uuids::uuid::from_string(*options.operation);
            if (!uuid) return void(exit_code = context.usage("--operation must be a UUID"));
            edit.operation = *uuid;
        }
        edit.sample_ids = options.samples;
        edit.rating = options.rating;
        if (options.favorite) edit.favorite = *options.favorite == "yes";
        edit.notes = options.notes;
        edit.display_name = options.name;
        edit.tags_add = options.tags_add;
        edit.tags_remove = options.tags_remove;
        if (options.category) edit.category = std::optional<std::string>(*options.category);
        if (options.clear_category) edit.category = std::optional<std::string>();
        if (options.bpm) {
            auto bpm = parse_bpm_edit(*options.bpm);
            if (!bpm) return void(exit_code = context.usage(bpm.error().message()));
            edit.bpm = *bpm;
        }
        if (options.key) {
            auto key = parse_key_edit(*options.key);
            if (!key) return void(exit_code = context.usage(key.error().message()));
            edit.key = *key;
        }
        edit.add_to_collection = options.add_to;
        edit.remove_from_collection = options.remove_from;
        if (options.remove) edit.removed = true;
        if (options.restore) edit.removed = false;
        const bool changes_something = edit.rating || edit.favorite || edit.notes || edit.display_name ||
                                       !edit.tags_add.empty() || !edit.tags_remove.empty() || edit.category ||
                                       edit.bpm || edit.key || edit.add_to_collection || edit.remove_from_collection ||
                                       edit.removed;
        if (!changes_something) return void(exit_code = context.usage("mso edit: name at least one change to make"));

        auto library = context.open_library(path_from_utf8(options.directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto receipt = (*context.application())->apply_edit(library->uuid, edit);
        if (!receipt) return void(exit_code = context.report(receipt.error()));
        exit_code = print_receipt(context, *receipt);
    });
}

/// Registers `tag`.
void register_tag(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("tag", "Rename, move or merge a tag (undoable)");
    /// Every option of `tag`.
    struct Options {
        std::string directory;
        std::int64_t tag = 0;
        std::string rename;
        std::optional<std::int64_t> move_under;
        std::optional<std::int64_t> merge_into;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    command->add_option("tag", options.tag, "Tag id (see `mso tags`)")->required();
    auto* rename_option = command->add_option("--rename", options.rename, "New name");
    auto* move_option = command->add_option("--move-under", options.move_under, "New parent tag id");
    auto* top_option = command->add_flag("--move-to-top", "Make it a top-level tag");  // read through count()
    auto* merge_option = command->add_option("--merge-into", options.merge_into, "Merge into this tag id");
    const std::vector<CLI::Option*> actions = {rename_option, move_option, top_option, merge_option};
    for (auto* action : actions)
        for (auto* other : actions)
            if (action != other) action->excludes(other);
    command->callback([&, actions] {
        if (std::ranges::none_of(actions, [](const CLI::Option* action) { return action->count() > 0; }))
            return void(exit_code = context.usage(
                            "mso tag: choose one of --rename, --move-under, --move-to-top or --merge-into"));
        catalog::TaxonomyCommand command_value{.target_id = options.tag};
        if (actions[0]->count() > 0) {
            command_value.kind = catalog::TaxonomyCommand::Kind::RenameTag;
            command_value.new_name = options.rename;
        } else if (actions[1]->count() > 0 || actions[2]->count() > 0) {
            command_value.kind = catalog::TaxonomyCommand::Kind::MoveTag;
            command_value.new_parent_id = options.move_under;  // nullopt with --move-to-top
        } else {
            command_value.kind = catalog::TaxonomyCommand::Kind::MergeTag;
            command_value.other_id = options.merge_into;
        }
        auto library = context.open_library(path_from_utf8(options.directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto receipt = (*context.application())->apply_taxonomy(library->uuid, command_value);
        if (!receipt) return void(exit_code = context.report(receipt.error()));
        exit_code = print_receipt(context, *receipt);
    });
}

/// Registers `collection`.
void register_collection(CLI::App& root, CommandContext& context, int& exit_code) {
    auto* command = root.add_subcommand("collection", "Create, rename or delete a collection (undoable)");
    /// Every option of `collection`.
    struct Options {
        std::string directory;
        std::string create;
        std::string smart;
        std::int64_t target = 0;
        std::string rename;
    };
    auto& options = context.command_options<Options>();
    command->add_option("library", options.directory, "Library folder")->required();
    auto* create_option = command->add_option("--create", options.create, "Name of a new collection");
    auto* smart_option =
        command->add_option("--smart", options.smart, "With --create: the query of a smart collection");
    auto* id_option = command->add_option("--id", options.target, "Existing collection id");
    auto* rename_option = command->add_option("--rename", options.rename, "New name (with --id)");
    // No bound variable: the callback reads count().
    auto* delete_option = command->add_flag("--delete", "Delete (with --id; memberships are kept for undo)");
    smart_option->needs(create_option);
    create_option->excludes(id_option);
    rename_option->needs(id_option);
    delete_option->needs(id_option);
    rename_option->excludes(delete_option);
    command->callback([&, create_option, rename_option, delete_option] {
        catalog::TaxonomyCommand command_value{.target_id = options.target};
        if (create_option->count() > 0) {
            if (!options.smart.empty())
                if (auto valid = search::SearchService().validate(options.smart); !valid)
                    return void(exit_code = context.usage(valid.error().message()));
            command_value.kind = catalog::TaxonomyCommand::Kind::CreateCollection;
            command_value.new_name = options.create;
            if (!options.smart.empty()) command_value.smart_query = options.smart;
        } else if (rename_option->count() > 0) {
            command_value.kind = catalog::TaxonomyCommand::Kind::RenameCollection;
            command_value.new_name = options.rename;
        } else if (delete_option->count() > 0) {
            command_value.kind = catalog::TaxonomyCommand::Kind::DeleteCollection;
        } else {
            return void(exit_code =
                            context.usage("mso collection: choose --create, or --id with --rename or --delete"));
        }
        auto library = context.open_library(path_from_utf8(options.directory), Access::Write);
        if (!library) return void(exit_code = context.report(library.error()));
        auto receipt = (*context.application())->apply_taxonomy(library->uuid, command_value);
        if (!receipt) return void(exit_code = context.report(receipt.error()));
        exit_code = print_receipt(context, *receipt);
    });
}

}  // namespace

void register_edit_commands(CLI::App& root, CommandContext& context, int& exit_code) {
    register_edit(root, context, exit_code);
    register_tag(root, context, exit_code);
    register_collection(root, context, exit_code);
}

}  // namespace mso::cli
