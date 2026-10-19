#pragma once
/// @file
/// PreferencesDialog: general preview behavior and customizable keyboard shortcuts with
/// conflict detection (R17).
#include <QDialog>

#include <vector>

#include "mso/ui/shortcut_catalog.hpp"

class QCheckBox;
class QDialogButtonBox;
class QKeySequenceEdit;
class QLabel;
class QTableWidget;

namespace mso::ui {

/// Modal preferences editor. OK stays disabled while two commands share a key sequence.
class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    PreferencesDialog(const ShortcutBindings& bindings, bool audition_on_selection, QWidget* parent = nullptr);

    ShortcutBindings bindings() const;
    bool audition_on_selection() const;

private:
    /// Highlights conflicting rows and enables OK only without conflicts.
    void check_conflicts();
    void load_bindings(const ShortcutBindings& bindings);

    QCheckBox* audition_check_ = nullptr;
    QTableWidget* shortcut_table_ = nullptr;
    std::vector<QKeySequenceEdit*> sequence_edits_;  ///< parallel to ShortcutCatalog::definitions()
    QLabel* conflict_label_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace mso::ui
