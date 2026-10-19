#include "mso/ui/add_folder_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace mso::ui {

AddFolderDialog::AddFolderDialog(const QString& folder, const std::vector<std::pair<QString, uuids::uuid>>& libraries,
                                 const std::optional<uuids::uuid>& preselected, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Add folder"));

    auto* folder_label = new QLabel(folder, this);
    folder_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folder_label->setWordWrap(true);

    library_combo_ = new QComboBox(this);
    library_combo_->setAccessibleName(tr("Target library"));
    for (const auto& [name, id] : libraries) {
        library_combo_->addItem(name);
        library_ids_.push_back(id);
        if (preselected && id == *preselected) library_combo_->setCurrentIndex(library_combo_->count() - 1);
    }

    auto* reference = new QRadioButton(tr("Reference in place — files stay where they are (recommended)"), this);
    reference->setChecked(true);
    auto* copy = new QRadioButton(tr("Copy into library — coming later"), this);
    copy->setEnabled(false);
    copy->setToolTip(tr("Managed copies are not available in this version yet."));

    auto* note = new QLabel(tr("The folder is scanned now (file names and container metadata). Waveforms, BPM "
                               "and key are then analyzed automatically in the background; you can pause or "
                               "cancel that in the Jobs panel."),
                            this);
    note->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add and import"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!libraries.empty());
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* form = new QFormLayout;
    form->addRow(tr("Folder"), folder_label);
    form->addRow(tr("Library"), library_combo_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(reference);
    layout->addWidget(copy);
    layout->addWidget(note);
    layout->addWidget(buttons);
}

std::optional<uuids::uuid> AddFolderDialog::library() const {
    const int index = library_combo_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= library_ids_.size()) return std::nullopt;
    return library_ids_[static_cast<std::size_t>(index)];
}

}  // namespace mso::ui
