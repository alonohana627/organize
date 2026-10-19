#include "mso/ui/preferences_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <set>

namespace mso::ui {

PreferencesDialog::PreferencesDialog(const ShortcutBindings& bindings, bool audition_on_selection, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Preferences"));
    resize(560, 520);

    auto* general = new QWidget(this);
    audition_check_ = new QCheckBox(tr("Audition on selection (play a sample as soon as it is selected)"), general);
    audition_check_->setChecked(audition_on_selection);
    auto* general_layout = new QVBoxLayout(general);
    general_layout->addWidget(audition_check_);
    general_layout->addStretch(1);

    auto* shortcuts = new QWidget(this);
    const std::vector<ShortcutDefinition> definitions = ShortcutCatalog::definitions();
    shortcut_table_ = new QTableWidget(static_cast<int>(definitions.size()), 2, shortcuts);
    shortcut_table_->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
    shortcut_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcut_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    shortcut_table_->verticalHeader()->hide();
    shortcut_table_->setSelectionMode(QAbstractItemView::NoSelection);
    shortcut_table_->setAccessibleName(tr("Keyboard shortcuts"));
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        auto* label = new QTableWidgetItem(definitions[row].label);
        label->setFlags(Qt::ItemIsEnabled);
        shortcut_table_->setItem(static_cast<int>(row), 0, label);
        auto* edit = new QKeySequenceEdit(shortcut_table_);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        edit->setAccessibleName(tr("Shortcut for %1").arg(definitions[row].label));
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, &PreferencesDialog::check_conflicts);
        shortcut_table_->setCellWidget(static_cast<int>(row), 1, edit);
        sequence_edits_.push_back(edit);
    }
    conflict_label_ = new QLabel(shortcuts);
    conflict_label_->setWordWrap(true);
    auto* reset_button = new QPushButton(tr("Restore defaults"), shortcuts);
    connect(reset_button, &QPushButton::clicked, this, [this] { load_bindings(ShortcutCatalog::defaults()); });
    auto* shortcuts_layout = new QVBoxLayout(shortcuts);
    shortcuts_layout->addWidget(shortcut_table_, 1);
    shortcuts_layout->addWidget(conflict_label_);
    shortcuts_layout->addWidget(reset_button, 0, Qt::AlignLeft);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(general, tr("General"));
    tabs->addTab(shortcuts, tr("Keyboard shortcuts"));

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons_);
    load_bindings(bindings);
}

void PreferencesDialog::load_bindings(const ShortcutBindings& bindings) {
    const std::vector<ShortcutDefinition> definitions = ShortcutCatalog::definitions();
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        const auto found = bindings.find(definitions[row].id);
        sequence_edits_[row]->setKeySequence(found != bindings.end() ? found->second
                                                                     : definitions[row].default_sequence);
    }
    check_conflicts();
}

ShortcutBindings PreferencesDialog::bindings() const {
    ShortcutBindings result;
    const std::vector<ShortcutDefinition> definitions = ShortcutCatalog::definitions();
    for (std::size_t row = 0; row < definitions.size(); ++row)
        result[definitions[row].id] = sequence_edits_[row]->keySequence();
    return result;
}

bool PreferencesDialog::audition_on_selection() const {
    return audition_check_->isChecked();
}

void PreferencesDialog::check_conflicts() {
    const auto groups = ShortcutCatalog::conflicts(bindings());
    std::set<QString> conflicting;
    QStringList descriptions;
    for (const auto& group : groups) {
        QStringList labels;
        for (const QString& id : group) {
            conflicting.insert(id);
            labels << ShortcutCatalog::label(id);
        }
        descriptions << QLocale().createSeparatedList(labels);  // "A and B", localized
    }
    const std::vector<ShortcutDefinition> definitions = ShortcutCatalog::definitions();
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        QTableWidgetItem* label = shortcut_table_->item(static_cast<int>(row), 0);
        const bool conflict = conflicting.contains(definitions[row].id);
        label->setText(conflict ? tr("⚠ %1").arg(definitions[row].label) : definitions[row].label);
        label->setToolTip(conflict ? tr("Shares its shortcut with another command") : QString());
    }
    conflict_label_->setText(descriptions.isEmpty() ? QString()
                                                    : tr("Conflicting shortcuts: %1. Change one of them to continue.")
                                                          .arg(descriptions.join(QStringLiteral("; "))));
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(descriptions.isEmpty());
}

}  // namespace mso::ui
