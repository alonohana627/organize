#include "mso/catalog/edit_command.hpp"

#include <algorithm>
#include <format>

namespace mso::catalog {

namespace {
constexpr std::size_t kMaxNotesBytes = 64 * 1024;
constexpr std::size_t kMaxDisplayNameCodePoints = 1024;
constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;

bool has_any_change(const EditCommand& command) {
    return command.rating || command.favorite || command.notes || command.display_name || !command.tags_add.empty() ||
           !command.tags_remove.empty() || command.category || command.bpm || command.key ||
           command.add_to_collection || command.remove_from_collection || command.removed;
}
}  // namespace

Result<void> validate(const EditCommand& command) {
    if (command.sample_ids.empty()) return fail(ErrorCode::InvalidArgument, "no samples selected");
    if (!has_any_change(command)) return fail(ErrorCode::InvalidArgument, "the edit changes nothing");
    if (command.rating && (*command.rating < 0 || *command.rating > 5))
        return fail(ErrorCode::InvalidArgument, "rating must be between 0 and 5");
    if (command.notes && (command.notes->size() > kMaxNotesBytes || !is_valid_utf8(*command.notes)))
        return fail(ErrorCode::InvalidArgument, "notes must be valid UTF-8 and at most 64 KiB");
    if (command.display_name) {
        const auto length = count_code_points(*command.display_name);
        if (trim(*command.display_name).empty() || length > kMaxDisplayNameCodePoints ||
            !is_valid_utf8(*command.display_name))
            return fail(ErrorCode::InvalidArgument, "display name must be 1 to 1024 characters of valid UTF-8");
    }
    for (const auto& added : command.tags_add) {
        const bool also_removed = std::ranges::any_of(
            command.tags_remove, [&](const std::string& removed) { return fold_key(removed) == fold_key(added); });
        if (also_removed)
            return fail(ErrorCode::InvalidArgument, std::format("tag '{}' is both added and removed", added));
    }
    if (command.add_to_collection && command.remove_from_collection &&
        *command.add_to_collection == *command.remove_from_collection)
        return fail(ErrorCode::InvalidArgument, "the same collection is both added to and removed from");
    if (command.bpm) {
        const bool needs_value = command.bpm->state == OverrideState::Value;
        if (needs_value != command.bpm->milli_bpm.has_value())
            return fail(ErrorCode::InvalidArgument, "a BPM value is required exactly when the override is a value");
        if (needs_value && (*command.bpm->milli_bpm < kMinimumMilliBpm || *command.bpm->milli_bpm > kMaximumMilliBpm))
            return fail(ErrorCode::InvalidArgument, "BPM must be between 20 and 400");
    }
    if (command.key) {
        const bool needs_value = command.key->state == OverrideState::Value;
        if (needs_value != command.key->key.has_value())
            return fail(ErrorCode::InvalidArgument, "a key is required exactly when the override is a value");
        if (needs_value && (command.key->key->pitch_class < 0 || command.key->key->pitch_class > 11))
            return fail(ErrorCode::InvalidArgument, "pitch class must be between 0 and 11");
    }
    return {};
}

std::string describe(const EditCommand& command) {
    if (!command.label.empty()) return command.label;
    std::vector<std::string> parts;
    if (command.rating) parts.push_back(std::format("rating {}", *command.rating));
    if (command.favorite) parts.push_back(*command.favorite ? "favorite" : "unfavorite");
    if (command.notes) parts.push_back("notes");
    if (command.display_name) parts.push_back("rename");
    if (!command.tags_add.empty()) parts.push_back(std::format("add tag {}", command.tags_add.front()));
    if (!command.tags_remove.empty()) parts.push_back(std::format("remove tag {}", command.tags_remove.front()));
    if (command.category)
        parts.push_back(command.category->has_value() ? std::format("category {}", **command.category)
                                                      : std::string("clear category"));
    if (command.bpm) parts.push_back("BPM");
    if (command.key) parts.push_back("key");
    if (command.add_to_collection) parts.push_back("add to collection");
    if (command.remove_from_collection) parts.push_back("remove from collection");
    if (command.removed) parts.push_back(*command.removed ? "remove from library" : "restore");

    std::string label;
    for (const auto& part : parts) {
        if (!label.empty()) label += ", ";
        label += part;
    }
    const auto count = command.sample_ids.size();
    return std::format("{} on {} sample{}", label, count, count == 1 ? "" : "s");
}

}  // namespace mso::catalog
