#pragma once
/// @file
/// AddFolderDialog: confirms how a chosen source folder joins a library (docs/01 §5.1 step 3).
#include <QDialog>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

#include "mso/domain/core.hpp"

class QComboBox;

namespace mso::ui {

/// Asks which library receives the folder and how: "Reference in place" (the default and, for
/// now, the only mode) or "Copy into library", shown disabled until managed import lands.
class AddFolderDialog final : public QDialog {
    Q_OBJECT

public:
    /// `libraries` are (display name, UUID) pairs of the writable open libraries.
    AddFolderDialog(const QString& folder, const std::vector<std::pair<QString, uuids::uuid>>& libraries,
                    const std::optional<uuids::uuid>& preselected, QWidget* parent = nullptr);

    /// The library chosen by the user.
    std::optional<uuids::uuid> library() const;

private:
    /// Library of each combo entry, by position (names may repeat).
    std::vector<uuids::uuid> library_ids_;
    /// The target library choice.
    QComboBox* library_combo_ = nullptr;
};

}  // namespace mso::ui
