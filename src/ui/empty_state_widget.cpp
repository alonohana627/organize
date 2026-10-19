#include "mso/ui/empty_state_widget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace mso::ui {

EmptyStateWidget::EmptyStateWidget(QWidget* parent) : QWidget(parent) {
    title_label_ = new QLabel(this);
    QFont title_font = title_label_->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.6);
    title_font.setBold(true);
    title_label_->setFont(title_font);
    title_label_->setAlignment(Qt::AlignCenter);

    message_label_ = new QLabel(this);
    message_label_->setAlignment(Qt::AlignCenter);
    message_label_->setWordWrap(true);
    message_label_->setMaximumWidth(560);

    add_folder_button_ = new QPushButton(tr("Add sample folder…"), this);
    add_folder_button_->setDefault(true);
    add_folder_button_->setAccessibleDescription(
        tr("Choose a folder of samples; it is scanned and analyzed automatically"));
    open_button_ = new QPushButton(tr("Open existing library…"), this);
    open_button_->setAccessibleDescription(tr("Open a library folder created earlier"));
    connect(add_folder_button_, &QPushButton::clicked, this, &EmptyStateWidget::addFolderRequested);
    connect(open_button_, &QPushButton::clicked, this, &EmptyStateWidget::openLibraryRequested);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(add_folder_button_);
    buttons->addWidget(open_button_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->addStretch(1);
    layout->addWidget(title_label_);
    layout->addWidget(message_label_, 0, Qt::AlignHCenter);
    layout->addSpacing(16);
    layout->addLayout(buttons);
    layout->addStretch(2);
    show_busy(tr("Opening workspace…"));
}

void EmptyStateWidget::show_busy(const QString& message) {
    title_label_->setText(tr("Music Sample Organizer"));
    message_label_->setText(message);
    add_folder_button_->hide();
    open_button_->hide();
}

void EmptyStateWidget::show_welcome() {
    title_label_->setText(tr("Welcome to Music Sample Organizer"));
    message_label_->setText(
        tr("Add your sample folders: they are scanned and analyzed (BPM, key, waveforms, duplicates) "
           "automatically. Your audio files stay where they are, and everything works offline."));
    add_folder_button_->show();
    open_button_->show();
    add_folder_button_->setFocus();
}

void EmptyStateWidget::show_error(const QString& message) {
    show_welcome();
    message_label_->setText(message);
}

}  // namespace mso::ui
