#pragma once
/// @file
/// OptimisticEdit: how an EditCommand changes a visible table row before SQLite confirms it.
#include "mso/catalog/edit_command.hpp"
#include "mso/search/search_service.hpp"

namespace mso::ui {

/// Predicts the row an edit will produce, so the table and inspector update the instant the user
/// acts. The prediction covers what the row shows (rating, favorite, name, tags, category, manual
/// BPM/key); values only the catalog can compute (the automatic BPM after "Automatic", new tag
/// ids) are filled in when the edit is reconciled with Application::rows().
class OptimisticEdit {
public:
    /// Applies the user-visible effect of `command` to `row`.
    static void apply(const catalog::EditCommand& command, search::SampleRow& row);
    /// True when the command removes the samples from the library (their rows disappear).
    static bool removes_rows(const catalog::EditCommand& command);
};

}  // namespace mso::ui
