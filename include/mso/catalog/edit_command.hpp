#pragma once
/// @file
/// User metadata commands. Each command becomes one undoable history action.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mso/catalog/types.hpp"
#include "mso/domain/core.hpp"

namespace mso::catalog {

/// New BPM override: Automatic clears it, Unknown suppresses inference, Value pins `milli_bpm`.
struct BpmOverrideEdit {
    OverrideState state = OverrideState::Automatic;
    std::optional<std::int64_t> milli_bpm;
};

/// New key override, same tri-state semantics as BpmOverrideEdit.
struct KeyOverrideEdit {
    OverrideState state = OverrideState::Automatic;
    std::optional<MusicalKey> key;
};

/// Edits applied to every sample in `sample_ids`. Fields left empty are not touched, so a
/// multi-selection never overwrites values the user did not explicitly set.
/// Large selections are committed in chunks of at most kSamplesPerChunk; a crash or cancel keeps
/// the committed chunks as one partially-applied, undoable action; so does a failed chunk, and
/// retrying the same operation id resumes after the last committed chunk (Library::apply_edit).
struct EditCommand {
    static constexpr std::size_t kSamplesPerChunk = 500;

    uuids::uuid operation = new_uuid();  ///< idempotency key; replaying it returns the stored receipt
    std::string label;                   ///< shown in the history list; derived when empty
    std::vector<std::int64_t> sample_ids;

    std::optional<int> rating;  ///< 0..5, 0 = unrated
    std::optional<bool> favorite;
    std::optional<std::string> notes;                    ///< up to 64 KiB UTF-8
    std::optional<std::string> display_name;             ///< 1..1024 code points
    std::vector<std::string> tags_add;                   ///< tag paths ("Drums/Kick"); missing tags are created
    std::vector<std::string> tags_remove;                ///< tag paths; unknown paths are ignored
    std::optional<std::optional<std::string>> category;  ///< set a category path, or clear with nullopt
    std::optional<BpmOverrideEdit> bpm;
    std::optional<KeyOverrideEdit> key;
    std::optional<std::int64_t> add_to_collection;
    std::optional<std::int64_t> remove_from_collection;
    std::optional<bool> removed;  ///< true = soft-remove from the catalog (tombstone), false = restore
};

/// Structural edits of tags, categories and collections; also undoable history actions.
struct TaxonomyCommand {
    enum class Kind {
        RenameTag,         ///< target_id, new_name
        MoveTag,           ///< target_id, new_parent_id (nullopt = top level)
        MergeTag,          ///< target_id (source) merged into other_id (canonical)
        RenameCategory,    ///< target_id, new_name
        CreateCollection,  ///< new_name, smart_query (nullopt = manual)
        RenameCollection,  ///< target_id, new_name
        DeleteCollection,  ///< target_id (soft delete)
    };
    Kind kind = Kind::RenameTag;
    uuids::uuid operation = new_uuid();
    std::int64_t target_id = 0;
    std::optional<std::int64_t> other_id;
    std::optional<std::int64_t> new_parent_id;
    std::string new_name;
    std::optional<std::string> smart_query;
};

/// Checks value bounds and incompatible operations before any write.
Result<void> validate(const EditCommand& command);

/// A short history label such as "Set rating 4 on 12 samples".
std::string describe(const EditCommand& command);

}  // namespace mso::catalog
