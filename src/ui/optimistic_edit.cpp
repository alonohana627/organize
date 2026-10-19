#include "mso/ui/optimistic_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace mso::ui {

namespace {

/// Removes the tag `path` (and its id, kept parallel) from the row.
void remove_tag(search::SampleRow& row, const std::string& path) {
    const auto found = std::ranges::find(row.tag_paths, path);
    if (found == row.tag_paths.end()) return;
    const auto position = static_cast<std::size_t>(found - row.tag_paths.begin());
    row.tag_paths.erase(found);
    if (position < row.tag_ids.size()) row.tag_ids.erase(row.tag_ids.begin() + static_cast<std::ptrdiff_t>(position));
}

/// Adds the tag `path` unless present; its id is unknown (0) until the edit is reconciled.
void add_tag(search::SampleRow& row, const std::string& path) {
    if (std::ranges::find(row.tag_paths, path) != row.tag_paths.end()) return;
    row.tag_paths.push_back(path);
    row.tag_ids.push_back(0);
}

}  // namespace

void OptimisticEdit::apply(const catalog::EditCommand& command, search::SampleRow& row) {
    if (command.rating) row.rating = *command.rating;
    if (command.favorite) row.favorite = *command.favorite;
    if (command.display_name) row.display_name = *command.display_name;
    if (!command.tags_add.empty() || !command.tags_remove.empty()) {
        for (const std::string& path : command.tags_remove)
            remove_tag(row, path);
        for (const std::string& path : command.tags_add)
            add_tag(row, path);
    }
    if (command.category) {
        row.category = command.category->value_or(std::string());
        row.category_id.reset();  // the catalog assigns (or creates) the id; reconciliation fills it in
    }
    if (command.bpm) {
        if (command.bpm->state == catalog::OverrideState::Value) {
            row.bpm_milli = command.bpm->milli_bpm;
            row.bpm_source = "user";
            row.bpm_confidence.reset();
        } else if (command.bpm->state == catalog::OverrideState::Unknown) {
            row.bpm_milli.reset();
            row.bpm_source = "user_unknown";
            row.bpm_confidence.reset();
        }  // Automatic: the inferred value is only known after the catalog recomputes it
    }
    if (command.key) {
        if (command.key->state == catalog::OverrideState::Value) {
            row.key = command.key->key;
            row.key_source = "user";
            row.key_confidence.reset();
        } else if (command.key->state == catalog::OverrideState::Unknown) {
            row.key.reset();
            row.key_source = "user_unknown";
            row.key_confidence.reset();
        }
    }
}

bool OptimisticEdit::removes_rows(const catalog::EditCommand& command) {
    return command.removed.value_or(false);
}

}  // namespace mso::ui
