#include "mso/ui/empty_state_widget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace mso::ui {

EmptyStateWidget::EmptyStateWidget(QWidget* parent) : QWidget(parent) {
    auto* page = new QWidget;
    title_label_ = new QLabel(page);
    QFont title_font = title_label_->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.6);
    title_font.setBold(true);
    title_label_->setFont(title_font);
    title_label_->setAlignment(Qt::AlignCenter);
    title_label_->setWordWrap(true);

    message_label_ = new QLabel(page);
    message_label_->setObjectName(QStringLiteral("EmptyStateMessage"));
    message_label_->setAlignment(Qt::AlignCenter);
    message_label_->setWordWrap(true);
    message_label_->setTextFormat(Qt::PlainText);  // error messages quote paths and names
    message_label_->setMaximumWidth(kMaximumMessageWidth);
    // The label is centred by stretches, never by an alignment flag on the layout item: an aligned
    // item gets its size hint instead of height-for-width, and a wrapped message lost its last
    // lines ("… waveforms,").
    QSizePolicy message_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    message_policy.setHeightForWidth(true);
    message_label_->setSizePolicy(message_policy);
    auto* message_row = new QHBoxLayout;
    message_row->addStretch(1);
    message_row->addWidget(message_label_, 100);
    message_row->addStretch(1);

    add_folder_button_ = new QPushButton(tr("Add sample folder…"), page);
    add_folder_button_->setDefault(true);
    add_folder_button_->setAccessibleDescription(
        tr("Choose a folder of samples; it is scanned and analyzed automatically"));
    open_button_ = new QPushButton(tr("Open existing library…"), page);
    open_button_->setAccessibleDescription(tr("Open a library folder created earlier"));
    connect(add_folder_button_, &QPushButton::clicked, this, &EmptyStateWidget::addFolderRequested);
    connect(open_button_, &QPushButton::clicked, this, &EmptyStateWidget::openLibraryRequested);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(add_folder_button_);
    buttons->addWidget(open_button_);
    buttons->addStretch(1);

    auto* page_layout = new QVBoxLayout(page);
    page_layout->addStretch(1);
    page_layout->addWidget(title_label_);
    page_layout->addLayout(message_row);
    page_layout->addSpacing(16);
    page_layout->addLayout(buttons);
    page_layout->addStretch(2);

    // A window too small for the wrapped text scrolls the page rather than clipping the message.
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
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
