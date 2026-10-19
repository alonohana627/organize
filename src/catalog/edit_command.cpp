#include "mso/catalog/edit_command.hpp"

#include <algorithm>
#include <format>

#include "mso/domain/taxonomy.hpp"

namespace mso::catalog {

namespace {
constexpr std::size_t kMaximumNotesBytes = 64 * 1024;
constexpr std::size_t kMaximumDisplayNameCodePoints = 1024;
constexpr std::int64_t kMinimumMilliBpm = 20'000;
constexpr std::int64_t kMaximumMilliBpm = 400'000;
/// Tag paths named in a generated history label before "+N more".
constexpr std::size_t kLabelTagNames = 3;

bool has_any_change(const EditCommand& command) {
    return command.rating || command.favorite || command.notes || command.display_name || !command.tags_add.empty() ||
           !command.tags_remove.empty() || command.category || command.bpm || command.key ||
           command.add_to_collection || command.remove_from_collection || command.removed;
}

/// Comparison key of a taxonomy path: each trimmed segment folded, joined with '/', so
/// "Drums / Kick" and "drums/kick" are the same tag.
Result<std::string> taxonomy_key(std::string_view path) {
    auto segments = split_taxonomy_path(path);
    if (!segments) return std::unexpected(segments.error());
    std::string key;
    for (const auto& segment : *segments) {
        if (!key.empty()) key += '/';
        key += fold_key(segment);
    }
    return key;
}

/// "Drums/Kick, Snare +2 more"
std::string tag_list(const std::vector<std::string>& paths) {
    std::string text;
    for (std::size_t index = 0; index < paths.size() && index < kLabelTagNames; ++index) {
        if (!text.empty()) text += ", ";
        text += trim(paths[index]);
    }
    if (paths.size() > kLabelTagNames) text += std::format(" +{} more", paths.size() - kLabelTagNames);
    return text;
}

std::string override_text(OverrideState state, const std::string& value) {
    switch (state) {
        case OverrideState::Value:
            return value;
        case OverrideState::Unknown:
            return "unknown";
        case OverrideState::Automatic:
            return "automatic";
    }
    return "automatic";
}

}  // namespace

Result<void> validate(const EditCommand& command) {
    if (command.sample_ids.empty()) return fail(ErrorCode::InvalidArgument, "no samples selected");
    if (!has_any_change(command)) return fail(ErrorCode::InvalidArgument, "the edit changes nothing");
    if (command.rating && (*command.rating < 0 || *command.rating > 5))
        return fail(ErrorCode::InvalidArgument, "rating must be between 0 and 5");
    if (command.notes && (command.notes->size() > kMaximumNotesBytes || !is_valid_utf8(*command.notes)))
        return fail(ErrorCode::InvalidArgument, "notes must be valid UTF-8 and at most 64 KiB");
    if (command.display_name) {
        if (!is_valid_utf8(*command.display_name) || trim(*command.display_name).empty() ||
            count_code_points(*command.display_name) > kMaximumDisplayNameCodePoints)
            return fail(ErrorCode::InvalidArgument, "display name must be 1 to 1024 characters of valid UTF-8");
    }
    std::vector<std::string> added_keys;
    for (const auto& added : command.tags_add) {
        auto key = taxonomy_key(added);
        if (!key) return std::unexpected(key.error());
        added_keys.push_back(std::move(*key));
    }
    for (const auto& removed : command.tags_remove) {
        auto key = taxonomy_key(removed);
        if (!key) return std::unexpected(key.error());
        if (std::ranges::find(added_keys, *key) != added_keys.end())
            return fail(ErrorCode::InvalidArgument, std::format("tag '{}' is both added and removed", removed));
    }
    if (command.category && command.category->has_value())
        if (auto key = taxonomy_key(**command.category); !key) return std::unexpected(key.error());
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
    if (command.removed && !*command.removed) parts.emplace_back("restore");
    if (command.rating) parts.push_back(std::format("set rating {}", *command.rating));
    if (command.favorite) parts.emplace_back(*command.favorite ? "mark favorite" : "unmark favorite");
    if (command.notes) parts.emplace_back("edit notes");
    if (command.display_name) parts.push_back(std::format("rename to '{}'", trim(*command.display_name)));
    if (command.category)
        parts.push_back(command.category->has_value() ? std::format("set category {}", trim(**command.category))
                                                      : std::string("clear category"));
    if (command.bpm)
        parts.push_back(std::format(
            "set BPM {}",
            override_text(command.bpm->state, command.bpm->milli_bpm ? format_bpm(*command.bpm->milli_bpm) : "")));
    if (command.key)
        parts.push_back(std::format(
            "set key {}", override_text(command.key->state, command.key->key ? format_key(*command.key->key) : "")));
    if (!command.tags_add.empty())
        parts.push_back(
            std::format("add tag{} {}", command.tags_add.size() == 1 ? "" : "s", tag_list(command.tags_add)));
    if (!command.tags_remove.empty())
        parts.push_back(
            std::format("remove tag{} {}", command.tags_remove.size() == 1 ? "" : "s", tag_list(command.tags_remove)));
    if (command.add_to_collection) parts.emplace_back("add to collection");
    if (command.remove_from_collection) parts.emplace_back("remove from collection");
    if (command.removed && *command.removed) parts.emplace_back("remove from library");

    std::string label;
    for (const auto& part : parts) {
        if (!label.empty()) label += ", ";
        label += part;
    }
    if (!label.empty() && label.front() >= 'a' && label.front() <= 'z')
        label.front() = static_cast<char>(label.front() - 'a' + 'A');
    const auto count = command.sample_ids.size();
    return std::format("{} on {} sample{}", label, count, count == 1 ? "" : "s");
}

}  // namespace mso::catalog
