#pragma once
/// @file
/// EditController: optimistic metadata edits, taxonomy edits and undo/redo, behind the
/// Application's edit API (contract "app → ui").
#include <QObject>
#include <QString>

#include <optional>
#include <vector>

#include "mso/catalog/edit_command.hpp"
#include "mso/ui/sample_key.hpp"

namespace mso::ui {

class EditGateway;
class SampleTableModel;

/// Makes edits feel instant. apply() changes the visible rows first (OptimisticEdit), then writes
/// one independent edit per library in the background (no cross-library atomicity, docs/01 §3).
/// When every library has answered, the rows are reconciled with the index (Application::rows) —
/// on success that shows values only the catalog computes, on failure it reverts the prediction —
/// and the outcome is reported. Several edits of the same row may be in flight; the model leaves
/// such rows alone until the last one settles.
class EditController final : public QObject {
    Q_OBJECT

public:
    /// Edits go through `gateway` and are shown in `model`; both must outlive the controller.
    EditController(EditGateway& gateway, SampleTableModel& model, QObject* parent = nullptr);

    /// Applies `command` to `keys` (the command's sample_ids are filled in per library).
    /// `done_message` describes the edit in the success report, e.g. "Rated 4".
    void apply(const std::vector<SampleKey>& keys, catalog::EditCommand command, const QString& done_message);
    /// Applies a structural edit; structureChanged() follows on success.
    void apply_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command);
    /// Undoes the latest action of `library`; affected rows refresh from the change event.
    void undo(const uuids::uuid& library);
    /// Redoes the latest undone action of `library`.
    void redo(const uuids::uuid& library);
    /// Library of the most recent edit (an undo routing fallback).
    std::optional<uuids::uuid> last_edited_library() const { return last_edited_library_; }

signals:
    /// An edit, undo or redo succeeded (short status text).
    void succeeded(const QString& message);
    /// An edit, undo or redo failed; the rows were restored. Shown prominently, not only in the
    /// status bar.
    void failed(const QString& title, const QString& details);
    /// The history of `library` changed (edit, undo, redo).
    void historyChanged(const uuids::uuid& library);
    /// Tags, categories or collections of `library` changed.
    void structureChanged(const uuids::uuid& library);
    /// Optimistically removed rows must come back after a failed removal: re-run the query.
    void reloadRequested();

private:
    /// Shared tail of undo() and redo().
    void undo_or_redo(const uuids::uuid& library, bool redo);

    /// Where edits are written.
    EditGateway& gateway_;
    /// Where edits are shown first.
    SampleTableModel& model_;
    /// Library of the most recent edit.
    std::optional<uuids::uuid> last_edited_library_;
};

}  // namespace mso::ui
